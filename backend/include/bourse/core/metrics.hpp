#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "bourse/core/common.hpp"

/// \file metrics.hpp
/// Lock-free counters/gauges and an HdrHistogram-style latency histogram.
///
/// Every number quoted in docs/benchmarks.md comes out of this file, so the
/// accuracy characteristics are stated explicitly rather than implied.

namespace bourse {

/// Monotonic counter. Padded to a cache line: `ops_total` and `errors_total`
/// are incremented by different threads on adjacent code paths, and without the
/// padding they would share a line and serialise on the coherence protocol.
class Counter {
 public:
  void increment(std::uint64_t delta = 1) noexcept { value_.fetch_add(delta, std::memory_order_relaxed); }

  [[nodiscard]] std::uint64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }

  void reset() noexcept { value_.store(0, std::memory_order_relaxed); }

 private:
  alignas(kCacheLineSize) std::atomic<std::uint64_t> value_{0};
  // Never read: its only job is to fill the rest of the cache line so the
  // next counter starts on a fresh one. Clang flags unread private fields,
  // and it is right that nothing uses this -- that is the point.
  [[maybe_unused]] char padding_[kCacheLineSize - sizeof(std::atomic<std::uint64_t>)]{};
};

class Gauge {
 public:
  void set(std::int64_t v) noexcept { value_.store(v, std::memory_order_relaxed); }

  void add(std::int64_t d) noexcept { value_.fetch_add(d, std::memory_order_relaxed); }

  void subtract(std::int64_t d) noexcept { value_.fetch_sub(d, std::memory_order_relaxed); }

  [[nodiscard]] std::int64_t value() const noexcept { return value_.load(std::memory_order_relaxed); }

 private:
  alignas(kCacheLineSize) std::atomic<std::int64_t> value_{0};
  [[maybe_unused]] char padding_[kCacheLineSize - sizeof(std::atomic<std::int64_t>)]{};
};

/// Fixed-bucket logarithmic histogram, the same scheme HdrHistogram uses.
///
/// A value is decomposed into (bucket, sub-bucket) where the bucket is the
/// magnitude (power of two) and the sub-bucket is a linear subdivision within
/// it. With kSubBucketBits = 4 there are 16 subdivisions per octave, bounding
/// the relative error of any reported percentile at 1/16 = 6.25%.
///
/// Recording is a single relaxed fetch_add with no allocation and no locking,
/// so it is safe to call from the matching-engine hot path.
class Histogram {
 public:
  static constexpr int kSubBucketBits = 4;
  static constexpr int kSubBucketCount = 1 << kSubBucketBits;   // 16
  static constexpr int kBucketCount = 64 - kSubBucketBits + 1;  // 61

  void record(std::uint64_t value) noexcept;

  [[nodiscard]] std::uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }

  [[nodiscard]] std::uint64_t sum() const noexcept { return sum_.load(std::memory_order_relaxed); }

  [[nodiscard]] std::uint64_t min() const noexcept;

  [[nodiscard]] std::uint64_t max() const noexcept { return max_.load(std::memory_order_relaxed); }

  [[nodiscard]] double mean() const noexcept;

  /// `percentile(99.0)` returns the smallest recorded magnitude at or below
  /// which 99% of samples fall, to within the 6.25% bucket resolution.
  [[nodiscard]] std::uint64_t percentile(double p) const noexcept;

  void reset() noexcept;

  struct Snapshot {
    std::uint64_t count = 0;
    std::uint64_t min = 0;
    std::uint64_t max = 0;
    double mean = 0.0;
    std::uint64_t p50 = 0;
    std::uint64_t p90 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t p999 = 0;
  };

  [[nodiscard]] Snapshot snapshot() const noexcept;

 private:
  static int bucketIndex(std::uint64_t value) noexcept;
  static int subBucketIndex(std::uint64_t value, int bucket) noexcept;
  static std::uint64_t valueAt(int bucket, int sub) noexcept;

  std::atomic<std::uint64_t> counts_[kBucketCount][kSubBucketCount]{};
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> sum_{0};
  std::atomic<std::uint64_t> max_{0};
};

/// Process-wide named metric registry.
///
/// Lookup takes a mutex, so callers resolve the handle once at construction and
/// keep the pointer -- never call `counter()` inside a loop.
class MetricsRegistry {
 public:
  static MetricsRegistry& instance();

  Counter& counter(const std::string& name);
  Gauge& gauge(const std::string& name);
  Histogram& histogram(const std::string& name);

  /// Prometheus text exposition format, served at GET /metrics.
  [[nodiscard]] std::string renderPrometheus() const;
  /// Compact JSON, consumed by the dashboard.
  [[nodiscard]] std::string renderJson() const;

  void reset();

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::unique_ptr<Counter>> counters_;
  std::map<std::string, std::unique_ptr<Gauge>> gauges_;
  std::map<std::string, std::unique_ptr<Histogram>> histograms_;
};

}  // namespace bourse
