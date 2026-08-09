#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "bourse/core/common.hpp"

namespace bourse {

/// Fixed-size worker pool with a **bounded** task queue.
///
/// The bound is the interesting part. An unbounded queue converts overload into
/// unbounded memory growth and then an OOM kill; a bounded queue converts it
/// into backpressure that propagates to the acceptor, which is the behaviour a
/// server actually wants. `submit()` blocks once the queue is full;
/// `tryScheduleTask()` fails fast instead, and that is what the HTTP layer uses
/// so it can answer 503 rather than stall the event loop.
class ThreadPool {
 public:
  explicit ThreadPool(std::size_t thread_count = 0, std::size_t max_queue_depth = 8192)
      : max_queue_depth_(max_queue_depth == 0 ? 1 : max_queue_depth) {
    if (thread_count == 0) {
      const unsigned hw = std::thread::hardware_concurrency();
      thread_count = hw == 0 ? 4 : static_cast<std::size_t>(hw);
    }
    running_.store(true, std::memory_order_release);
    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
      workers_.emplace_back([this] { workerLoop(); });
    }
  }

  ~ThreadPool() { shutdown(); }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  /// Blocking submit. Returns a future for the callable's result.
  template <typename Fn, typename... Args>
  auto submit(Fn&& fn, Args&&... args) -> std::future<std::invoke_result_t<Fn, Args...>> {
    using Ret = std::invoke_result_t<Fn, Args...>;

    auto task = std::make_shared<std::packaged_task<Ret()>>(
        [fn = std::forward<Fn>(fn), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Ret {
          return std::apply(std::move(fn), std::move(tup));
        });
    std::future<Ret> future = task->get_future();

    {
      std::unique_lock<std::mutex> lock(mutex_);
      not_full_.wait(lock, [this] {
        return queue_.size() < max_queue_depth_ || !running_.load(std::memory_order_acquire);
      });
      if (!running_.load(std::memory_order_acquire)) {
        throw std::runtime_error("ThreadPool::submit on a stopped pool");
      }
      queue_.emplace([task]() mutable { (*task)(); });
    }
    not_empty_.notify_one();
    return future;
  }

  /// Non-blocking fire-and-forget submit. Returns false when the queue is at
  /// its bound, letting the caller shed load deliberately.
  bool tryScheduleTask(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_.load(std::memory_order_acquire) || queue_.size() >= max_queue_depth_) {
        return false;
      }
      queue_.emplace(std::move(task));
    }
    not_empty_.notify_one();
    return true;
  }

  /// Blocks until every already-queued task has run. Does not stop the pool.
  void drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
  }

  void shutdown() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

  [[nodiscard]] std::size_t threadCount() const noexcept { return workers_.size(); }

  [[nodiscard]] std::size_t pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  [[nodiscard]] std::uint64_t completed() const noexcept {
    return completed_.load(std::memory_order_relaxed);
  }

 private:
  void workerLoop() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock,
                        [this] { return !queue_.empty() || !running_.load(std::memory_order_acquire); });
        if (queue_.empty()) {
          if (!running_.load(std::memory_order_acquire)) {
            return;
          }
          continue;
        }
        task = std::move(queue_.front());
        queue_.pop();
        ++active_;
      }
      not_full_.notify_one();

      // Exceptions must not escape into std::thread -- that is an immediate
      // std::terminate. Swallowing here keeps one bad handler from killing the
      // process; the packaged_task path already captures it into the future.
      try {
        task();
      } catch (...) {  // NOLINT(bugprone-empty-catch)
      }

      completed_.fetch_add(1, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        --active_;
        if (queue_.empty() && active_ == 0) {
          idle_.notify_all();
        }
      }
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::condition_variable idle_;
  std::queue<std::function<void()>> queue_;
  std::vector<std::thread> workers_;
  std::size_t max_queue_depth_;
  std::size_t active_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> completed_{0};
};

}  // namespace bourse
