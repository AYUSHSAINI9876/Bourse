#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bourse/core/result.hpp"
#include "bourse/net/poller.hpp"

namespace bourse::net {

/// Single-threaded reactor.
///
/// Everything registered with a loop is driven by exactly one thread, which is
/// what lets connection state be touched without any locking at all. Work
/// originating on other threads enters through `post()`, and the loop is woken
/// from its blocking `wait()` by writing to an eventfd (a self-pipe on
/// non-Linux platforms) that the loop itself has registered for readability.
///
/// The alternative -- a mutex around connection state so any thread can touch
/// it -- is what most first attempts do, and it converts a lock-free design
/// into one where a slow handler on one connection stalls every other.
class EventLoop {
 public:
  using ReadyCallback = std::function<void(std::uint32_t flags)>;
  using Task = std::function<void()>;

  EventLoop();
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  /// Registers a descriptor and returns its token. The token, not the fd, is
  /// what every later call refers to; see the note on `PollEvent::token`.
  Result<std::uint64_t> registerFd(int fd, std::uint32_t interest, ReadyCallback callback);
  Status updateInterest(std::uint64_t token, std::uint32_t interest);
  Status unregisterFd(std::uint64_t token);

  /// Runs until `stop()`. Must be called from the thread that owns the loop.
  void run();

  /// One iteration: wait, dispatch readiness, run expired timers, drain posted
  /// tasks. Exposed for tests, which need to step a loop deterministically.
  void runOnce(int timeout_ms);

  void stop();

  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

  /// Thread-safe. Runs `task` on the loop thread at the next opportunity. When
  /// called from the loop thread itself the task is still queued rather than
  /// run inline, which keeps re-entrancy out of handler code.
  void post(Task task);

  /// One-shot timer. Returns an id usable with `cancelTimer`.
  std::uint64_t scheduleAfter(std::int64_t delay_ms, Task task);
  /// Repeating timer, first firing after `interval_ms`.
  std::uint64_t scheduleEvery(std::int64_t interval_ms, Task task);
  void cancelTimer(std::uint64_t timer_id);

  [[nodiscard]] bool inLoopThread() const noexcept { return std::this_thread::get_id() == owner_; }

  /// Adopts the calling thread as the owner. Used by EventLoopThread, which
  /// constructs the loop on one thread and runs it on another.
  void bindToCurrentThread() noexcept { owner_ = std::this_thread::get_id(); }

  [[nodiscard]] std::string_view pollerName() const noexcept { return poller_->name(); }

  [[nodiscard]] std::uint64_t iterations() const noexcept {
    return iterations_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t registeredCount() const noexcept { return registrations_.size(); }

 private:
  struct Registration {
    int fd = -1;
    std::uint32_t interest = kNone;
    ReadyCallback callback;
  };

  struct Timer {
    std::int64_t deadline_ms = 0;
    std::uint64_t id = 0;
    std::int64_t interval_ms = 0;  ///< 0 for one-shot
    Task task;
  };

  struct TimerGreater {
    bool operator()(const Timer& a, const Timer& b) const noexcept { return a.deadline_ms > b.deadline_ms; }
  };

  void setupWakeup();
  void wakeup();
  void drainWakeup();
  void runPendingTasks();
  void runExpiredTimers();
  [[nodiscard]] int computeTimeout(int fallback_ms) const;

  std::unique_ptr<Poller> poller_;
  std::unordered_map<std::uint64_t, Registration> registrations_;
  std::vector<PollEvent> ready_;
  std::uint64_t next_token_ = 1;

  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> iterations_{0};
  std::thread::id owner_;

  std::mutex task_mutex_;
  std::vector<Task> pending_tasks_;
  std::atomic<bool> has_pending_{false};

  std::priority_queue<Timer, std::vector<Timer>, TimerGreater> timers_;
  std::unordered_set<std::uint64_t> cancelled_timers_;
  std::uint64_t next_timer_id_ = 1;

  int wakeup_fd_ = -1;
  int wakeup_write_fd_ = -1;  ///< differs from wakeup_fd_ only on the pipe path
  std::uint64_t wakeup_token_ = 0;
};

/// Owns a thread that runs one EventLoop. Blocks in the constructor until the
/// loop is up so callers can post work immediately after construction.
class EventLoopThread {
 public:
  explicit EventLoopThread(std::string name = "loop");
  ~EventLoopThread();

  EventLoopThread(const EventLoopThread&) = delete;
  EventLoopThread& operator=(const EventLoopThread&) = delete;

  [[nodiscard]] EventLoop& loop() noexcept { return *loop_; }

  void stop();

 private:
  std::unique_ptr<EventLoop> loop_;
  std::thread thread_;
  std::string name_;
  std::mutex mutex_;
  std::condition_variable started_;
  bool started_flag_ = false;
};

}  // namespace bourse::net
