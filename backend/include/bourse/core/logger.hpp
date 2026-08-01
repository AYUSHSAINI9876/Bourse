#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "bourse/core/common.hpp"

/// \file logger.hpp
/// Asynchronous, level-filtered logger.
///
/// Why asynchronous: the matching engine and the event loop both log on paths
/// where a blocking write(2) to a redirected stderr would show up directly in
/// the p99. Producers pay a mutex acquisition and a string move; a dedicated
/// consumer thread does the actual I/O.
///
/// Level filtering happens *before* the arguments are formatted, so a disabled
/// BOURSE_LOG_DEBUG costs one relaxed atomic load.

namespace bourse {

enum class LogLevel : std::uint8_t {
  kTrace = 0,
  kDebug = 1,
  kInfo = 2,
  kWarn = 3,
  kError = 4,
  kFatal = 5,
  kOff = 6,
};

[[nodiscard]] const char* toString(LogLevel level) noexcept;
[[nodiscard]] LogLevel parseLogLevel(std::string_view text, LogLevel fallback = LogLevel::kInfo) noexcept;

class Logger {
 public:
  using Sink = std::function<void(std::string_view line)>;

  static Logger& instance();

  void setLevel(LogLevel level) noexcept { level_.store(level, std::memory_order_relaxed); }
  [[nodiscard]] LogLevel level() const noexcept { return level_.load(std::memory_order_relaxed); }
  [[nodiscard]] bool enabled(LogLevel level) const noexcept {
    return static_cast<std::uint8_t>(level) >= static_cast<std::uint8_t>(this->level());
  }

  /// Replaces the destination. Must not be called while other threads are
  /// logging; intended for test setup and for wiring a file sink at startup.
  void setSink(Sink sink);

  /// Switches to synchronous delivery. Tests use this so that assertions can
  /// inspect captured output without racing the consumer thread.
  void setAsync(bool async);

  void submit(LogLevel level, const char* file, int line, std::string message);

  /// Blocks until the queue is drained. Called by the server on shutdown.
  void flush();

  ~Logger();

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

 private:
  Logger();

  void consumerLoop();
  void deliver(const std::string& line);
  [[nodiscard]] std::string format(LogLevel level, const char* file, int line, const std::string& message) const;

  std::atomic<LogLevel> level_{LogLevel::kInfo};
  std::atomic<bool> async_{false};
  std::atomic<bool> running_{false};

  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable drained_;
  std::deque<std::string> queue_;
  std::thread consumer_;
  Sink sink_;
};

namespace detail {

/// Streams every argument into one ostringstream. Chosen over std::format so
/// the project builds on any conforming C++20 standard library, including
/// GCC 11/12 where <format> is absent.
template <typename... Args>
[[nodiscard]] std::string concatMessage(Args&&... args) {
  std::ostringstream oss;
  (oss << ... << std::forward<Args>(args));
  return oss.str();
}

/// Trims the leading path so log lines show `net/event_loop.cpp` rather than an
/// absolute build path that differs between machines.
[[nodiscard]] const char* shortFileName(const char* path) noexcept;

}  // namespace detail

#define BOURSE_LOG(level, ...)                                                             \
  do {                                                                                     \
    ::bourse::Logger& _bourse_logger = ::bourse::Logger::instance();                        \
    if (_bourse_logger.enabled(level)) {                                                    \
      _bourse_logger.submit((level), __FILE__, __LINE__,                                    \
                            ::bourse::detail::concatMessage(__VA_ARGS__));                  \
    }                                                                                       \
  } while (false)

#define BOURSE_LOG_TRACE(...) BOURSE_LOG(::bourse::LogLevel::kTrace, __VA_ARGS__)
#define BOURSE_LOG_DEBUG(...) BOURSE_LOG(::bourse::LogLevel::kDebug, __VA_ARGS__)
#define BOURSE_LOG_INFO(...) BOURSE_LOG(::bourse::LogLevel::kInfo, __VA_ARGS__)
#define BOURSE_LOG_WARN(...) BOURSE_LOG(::bourse::LogLevel::kWarn, __VA_ARGS__)
#define BOURSE_LOG_ERROR(...) BOURSE_LOG(::bourse::LogLevel::kError, __VA_ARGS__)
#define BOURSE_LOG_FATAL(...) BOURSE_LOG(::bourse::LogLevel::kFatal, __VA_ARGS__)

}  // namespace bourse
