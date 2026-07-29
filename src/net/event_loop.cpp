#include "bourse/net/event_loop.hpp"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>

#include "bourse/core/clock.hpp"
#include "bourse/core/logger.hpp"

#if defined(BOURSE_PLATFORM_LINUX)
#include <sys/eventfd.h>
#include <unistd.h>
#elif !defined(BOURSE_PLATFORM_WINDOWS)
#include <unistd.h>
#endif

namespace bourse::net {

EventLoop::EventLoop() : poller_(Poller::create()), owner_(std::this_thread::get_id()) {
  ready_.reserve(64);
  setupWakeup();
}

EventLoop::~EventLoop() {
  stop();
#if !defined(BOURSE_PLATFORM_WINDOWS)
  if (wakeup_write_fd_ >= 0 && wakeup_write_fd_ != wakeup_fd_) {
    ::close(wakeup_write_fd_);
  }
  if (wakeup_fd_ >= 0) {
    ::close(wakeup_fd_);
  }
#endif
}

void EventLoop::setupWakeup() {
#if defined(BOURSE_PLATFORM_LINUX)
  // eventfd is a single descriptor with an 8-byte counter -- cheaper than a
  // pipe (one fd instead of two, no buffer management) and it collapses any
  // number of pending wakeups into one readable event.
  wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  wakeup_write_fd_ = wakeup_fd_;
#elif !defined(BOURSE_PLATFORM_WINDOWS)
  int fds[2] = {-1, -1};
  if (::pipe(fds) == 0) {
    wakeup_fd_ = fds[0];
    wakeup_write_fd_ = fds[1];
  }
#else
  // Windows has no pollable pipe; the loop falls back to a bounded wait so a
  // posted task is picked up within one tick instead of immediately.
  wakeup_fd_ = -1;
  wakeup_write_fd_ = -1;
#endif

  if (wakeup_fd_ < 0) {
    return;
  }

  Result<std::uint64_t> token = registerFd(wakeup_fd_, kReadable, [this](std::uint32_t) { drainWakeup(); });
  if (token.ok()) {
    wakeup_token_ = token.value();
  } else {
    BOURSE_LOG_ERROR("failed to register wakeup fd: ", token.status().toString());
  }
}

void EventLoop::wakeup() {
#if !defined(BOURSE_PLATFORM_WINDOWS)
  if (wakeup_write_fd_ < 0) {
    return;
  }
  const std::uint64_t one = 1;
  const ssize_t written = ::write(wakeup_write_fd_, &one, sizeof(one));
  if (written < 0 && errno != EAGAIN && errno != EINTR) {
    BOURSE_LOG_WARN("wakeup write failed: ", std::strerror(errno));
  }
#endif
}

void EventLoop::drainWakeup() {
#if !defined(BOURSE_PLATFORM_WINDOWS)
  std::uint64_t sink = 0;
  while (::read(wakeup_fd_, &sink, sizeof(sink)) > 0) {
    // Drain fully: on the pipe path several writers may have queued bytes.
  }
#endif
}

Result<std::uint64_t> EventLoop::registerFd(int fd, std::uint32_t interest, ReadyCallback callback) {
  if (fd < 0) {
    return Status::invalidArgument("cannot register a negative descriptor");
  }
  const std::uint64_t token = next_token_++;
  BOURSE_TRY(poller_->add(fd, interest, token));
  registrations_.emplace(token, Registration{fd, interest, std::move(callback)});
  return token;
}

Status EventLoop::updateInterest(std::uint64_t token, std::uint32_t interest) {
  auto it = registrations_.find(token);
  if (it == registrations_.end()) {
    return Status::notFound("unknown registration token");
  }
  if (it->second.interest == interest) {
    return Status::success();  // avoid a pointless syscall
  }
  BOURSE_TRY(poller_->modify(it->second.fd, interest, token));
  it->second.interest = interest;
  return Status::success();
}

Status EventLoop::unregisterFd(std::uint64_t token) {
  auto it = registrations_.find(token);
  if (it == registrations_.end()) {
    return Status::notFound("unknown registration token");
  }
  const Status removed = poller_->remove(it->second.fd);
  registrations_.erase(it);
  return removed;
}

void EventLoop::post(Task task) {
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    pending_tasks_.push_back(std::move(task));
  }
  has_pending_.store(true, std::memory_order_release);
  // Always wake, even from the loop thread: the loop may be about to block in
  // wait() with a long timeout computed before this task existed.
  wakeup();
}

std::uint64_t EventLoop::scheduleAfter(std::int64_t delay_ms, Task task) {
  const std::uint64_t id = next_timer_id_++;
  timers_.push(Timer{nowMillis() + delay_ms, id, 0, std::move(task)});
  return id;
}

std::uint64_t EventLoop::scheduleEvery(std::int64_t interval_ms, Task task) {
  const std::uint64_t id = next_timer_id_++;
  const std::int64_t safe_interval = interval_ms > 0 ? interval_ms : 1;
  timers_.push(Timer{nowMillis() + safe_interval, id, safe_interval, std::move(task)});
  return id;
}

void EventLoop::cancelTimer(std::uint64_t timer_id) { cancelled_timers_.insert(timer_id); }

int EventLoop::computeTimeout(int fallback_ms) const {
  if (timers_.empty()) {
    return fallback_ms;
  }
  const std::int64_t remaining = timers_.top().deadline_ms - nowMillis();
  if (remaining <= 0) {
    return 0;
  }
  return static_cast<int>(std::min<std::int64_t>(remaining, fallback_ms));
}

void EventLoop::runOnce(int timeout_ms) {
  const int effective_timeout = computeTimeout(timeout_ms);

  const int ready = poller_->wait(ready_, effective_timeout);
  if (ready < 0 && errno != EINTR) {
    BOURSE_LOG_ERROR("poller wait failed: ", std::strerror(errno));
  }

  for (const PollEvent& event : ready_) {
    // Resolving through the token map is what makes a stale kernel event safe:
    // if the connection was torn down since the wait returned, the lookup
    // simply misses instead of dereferencing freed memory.
    auto it = registrations_.find(event.token);
    if (it == registrations_.end()) {
      continue;
    }
    ReadyCallback callback = it->second.callback;
    if (callback) {
      callback(event.flags);
    }
  }

  runExpiredTimers();
  runPendingTasks();
  iterations_.fetch_add(1, std::memory_order_relaxed);
}

void EventLoop::runExpiredTimers() {
  const std::int64_t now = nowMillis();
  while (!timers_.empty() && timers_.top().deadline_ms <= now) {
    Timer timer = timers_.top();
    timers_.pop();

    if (cancelled_timers_.erase(timer.id) > 0) {
      continue;
    }
    if (timer.task) {
      timer.task();
    }
    if (timer.interval_ms > 0 && cancelled_timers_.count(timer.id) == 0) {
      timer.deadline_ms = now + timer.interval_ms;
      timers_.push(std::move(timer));
    }
  }
}

void EventLoop::runPendingTasks() {
  if (!has_pending_.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<Task> batch;
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    batch.swap(pending_tasks_);
    has_pending_.store(false, std::memory_order_release);
  }
  // Run outside the lock so a task that posts another task cannot deadlock.
  for (Task& task : batch) {
    if (task) {
      task();
    }
  }
}

void EventLoop::run() {
  owner_ = std::this_thread::get_id();
  running_.store(true, std::memory_order_release);
  BOURSE_LOG_INFO("event loop started (poller=", poller_->name(), ")");
  while (running_.load(std::memory_order_acquire)) {
    runOnce(50);
  }
  BOURSE_LOG_INFO("event loop stopped after ", iterations(), " iterations");
}

void EventLoop::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  wakeup();
}

// ---------------------------------------------------------------------------
// EventLoopThread
// ---------------------------------------------------------------------------

EventLoopThread::EventLoopThread(std::string name) : name_(std::move(name)) {
  thread_ = std::thread([this] {
    auto loop = std::make_unique<EventLoop>();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      loop_ = std::move(loop);
      started_flag_ = true;
    }
    started_.notify_all();
    loop_->run();
  });

  std::unique_lock<std::mutex> lock(mutex_);
  started_.wait(lock, [this] { return started_flag_; });
}

EventLoopThread::~EventLoopThread() { stop(); }

void EventLoopThread::stop() {
  if (loop_) {
    loop_->stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
}

}  // namespace bourse::net
