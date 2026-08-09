#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace bourse {

using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;
using Nanos = std::chrono::nanoseconds;
using Micros = std::chrono::microseconds;
using Millis = std::chrono::milliseconds;

/// Monotonic nanosecond counter. Used for latency measurement -- never for
/// wall-clock timestamps, because steady_clock has no defined epoch.
[[nodiscard]] inline std::int64_t nowNanos() noexcept {
  return std::chrono::duration_cast<Nanos>(SteadyClock::now().time_since_epoch()).count();
}

/// Wall-clock milliseconds since the Unix epoch. Used for TTLs and for anything
/// a client can observe, since those must survive a restart.
[[nodiscard]] inline std::int64_t nowMillis() noexcept {
  return std::chrono::duration_cast<Millis>(SystemClock::now().time_since_epoch()).count();
}

[[nodiscard]] inline std::int64_t nowMicros() noexcept {
  return std::chrono::duration_cast<Micros>(SystemClock::now().time_since_epoch()).count();
}

/// RAII stopwatch. Reports elapsed nanoseconds on demand; the destructor does
/// nothing, so it is safe to leave one on the stack in a hot path.
class Stopwatch {
 public:
  Stopwatch() noexcept : start_(SteadyClock::now()) {}

  void reset() noexcept { start_ = SteadyClock::now(); }

  [[nodiscard]] std::int64_t elapsedNanos() const noexcept {
    return std::chrono::duration_cast<Nanos>(SteadyClock::now() - start_).count();
  }

  [[nodiscard]] double elapsedMicros() const noexcept { return static_cast<double>(elapsedNanos()) / 1e3; }

  [[nodiscard]] double elapsedMillis() const noexcept { return static_cast<double>(elapsedNanos()) / 1e6; }

  [[nodiscard]] double elapsedSeconds() const noexcept { return static_cast<double>(elapsedNanos()) / 1e9; }

 private:
  SteadyClock::time_point start_;
};

/// `YYYY-MM-DD HH:MM:SS.mmm` in local time, for log lines.
[[nodiscard]] std::string formatTimestamp(std::int64_t epoch_millis);

}  // namespace bourse
