#include "bourse/core/metrics.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace bourse {

int Histogram::bucketIndex(std::uint64_t value) noexcept {
  // floor(log2(value)) + 1 - kSubBucketBits, clamped at zero. Values below
  // 2^kSubBucketBits all land in bucket 0 where the sub-bucket *is* the value,
  // giving exact resolution for small magnitudes.
  const int width = std::bit_width(value);  // 0 for value == 0
  const int index = width - kSubBucketBits;
  return index < 0 ? 0 : index;
}

int Histogram::subBucketIndex(std::uint64_t value, int bucket) noexcept {
  return static_cast<int>(value >> bucket);
}

std::uint64_t Histogram::valueAt(int bucket, int sub) noexcept {
  return static_cast<std::uint64_t>(sub) << bucket;
}

void Histogram::record(std::uint64_t value) noexcept {
  const int bucket = bucketIndex(value);
  int sub = subBucketIndex(value, bucket);
  if (sub >= kSubBucketCount) {
    sub = kSubBucketCount - 1;  // saturate rather than corrupt adjacent memory
  }
  counts_[bucket][sub].fetch_add(1, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  sum_.fetch_add(value, std::memory_order_relaxed);

  // Compare-and-swap loop for max. Contended only while a new maximum is
  // actually being established, which is rare after warm-up.
  std::uint64_t current_max = max_.load(std::memory_order_relaxed);
  while (value > current_max && !max_.compare_exchange_weak(current_max, value, std::memory_order_relaxed,
                                                            std::memory_order_relaxed)) {
  }
}

std::uint64_t Histogram::min() const noexcept {
  for (int bucket = 0; bucket < kBucketCount; ++bucket) {
    for (int sub = 0; sub < kSubBucketCount; ++sub) {
      if (counts_[bucket][sub].load(std::memory_order_relaxed) != 0) {
        return valueAt(bucket, sub);
      }
    }
  }
  return 0;
}

double Histogram::mean() const noexcept {
  const std::uint64_t n = count();
  if (n == 0) {
    return 0.0;
  }
  return static_cast<double>(sum()) / static_cast<double>(n);
}

std::uint64_t Histogram::percentile(double p) const noexcept {
  const std::uint64_t total = count();
  if (total == 0) {
    return 0;
  }
  const double clamped = std::clamp(p, 0.0, 100.0);
  auto target = static_cast<std::uint64_t>(std::ceil(clamped / 100.0 * static_cast<double>(total)));
  if (target == 0) {
    target = 1;
  }

  std::uint64_t running = 0;
  for (int bucket = 0; bucket < kBucketCount; ++bucket) {
    for (int sub = 0; sub < kSubBucketCount; ++sub) {
      running += counts_[bucket][sub].load(std::memory_order_relaxed);
      if (running >= target) {
        return valueAt(bucket, sub);
      }
    }
  }
  return max();
}

void Histogram::reset() noexcept {
  for (auto& bucket : counts_) {
    for (auto& slot : bucket) {
      slot.store(0, std::memory_order_relaxed);
    }
  }
  count_.store(0, std::memory_order_relaxed);
  sum_.store(0, std::memory_order_relaxed);
  max_.store(0, std::memory_order_relaxed);
}

Histogram::Snapshot Histogram::snapshot() const noexcept {
  Snapshot s;
  s.count = count();
  s.min = min();
  s.max = max();
  s.mean = mean();
  s.p50 = percentile(50.0);
  s.p90 = percentile(90.0);
  s.p99 = percentile(99.0);
  s.p999 = percentile(99.9);
  return s;
}

MetricsRegistry& MetricsRegistry::instance() {
  static MetricsRegistry registry;
  return registry;
}

Counter& MetricsRegistry::counter(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = counters_.find(name);
  if (it == counters_.end()) {
    it = counters_.emplace(name, std::make_unique<Counter>()).first;
  }
  return *it->second;
}

Gauge& MetricsRegistry::gauge(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = gauges_.find(name);
  if (it == gauges_.end()) {
    it = gauges_.emplace(name, std::make_unique<Gauge>()).first;
  }
  return *it->second;
}

Histogram& MetricsRegistry::histogram(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = histograms_.find(name);
  if (it == histograms_.end()) {
    it = histograms_.emplace(name, std::make_unique<Histogram>()).first;
  }
  return *it->second;
}

std::string MetricsRegistry::renderPrometheus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::ostringstream out;

  for (const auto& [name, counter] : counters_) {
    out << "# TYPE " << name << " counter\n";
    out << name << ' ' << counter->value() << '\n';
  }
  for (const auto& [name, gauge] : gauges_) {
    out << "# TYPE " << name << " gauge\n";
    out << name << ' ' << gauge->value() << '\n';
  }
  for (const auto& [name, histogram] : histograms_) {
    const Histogram::Snapshot s = histogram->snapshot();
    out << "# TYPE " << name << " summary\n";
    out << name << "{quantile=\"0.5\"} " << s.p50 << '\n';
    out << name << "{quantile=\"0.9\"} " << s.p90 << '\n';
    out << name << "{quantile=\"0.99\"} " << s.p99 << '\n';
    out << name << "{quantile=\"0.999\"} " << s.p999 << '\n';
    out << name << "_sum " << histogram->sum() << '\n';
    out << name << "_count " << s.count << '\n';
    out << name << "_max " << s.max << '\n';
  }
  return out.str();
}

std::string MetricsRegistry::renderJson() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::ostringstream out;
  out << '{';

  out << "\"counters\":{";
  bool first = true;
  for (const auto& [name, counter] : counters_) {
    if (!first)
      out << ',';
    first = false;
    out << '"' << name << "\":" << counter->value();
  }
  out << "},";

  out << "\"gauges\":{";
  first = true;
  for (const auto& [name, gauge] : gauges_) {
    if (!first)
      out << ',';
    first = false;
    out << '"' << name << "\":" << gauge->value();
  }
  out << "},";

  out << "\"histograms\":{";
  first = true;
  for (const auto& [name, histogram] : histograms_) {
    if (!first)
      out << ',';
    first = false;
    // One field per statement rather than a single twelve-term << chain.
    //
    // The chain was the only place in the tree where clang-format 18.1.3 and
    // 18.1.8 disagreed: LLVM changed how stream chains wrap in 18.1.4, so a
    // tree formatted by one version failed the check under the other. Short
    // statements give the formatter no wrapping decision to make, so every
    // 18.x agrees and the pinned version is belt-and-braces rather than
    // load-bearing.
    //
    // Deliberately not a table of {name, value} pairs: `mean` is a double and
    // the rest are uint64_t, so a single array would have to pick one type and
    // silently truncate the other.
    const Histogram::Snapshot s = histogram->snapshot();
    out << '"' << name << "\":{";
    out << "\"count\":" << s.count;
    out << ",\"min\":" << s.min;
    out << ",\"max\":" << s.max;
    out << ",\"mean\":" << s.mean;
    out << ",\"p50\":" << s.p50;
    out << ",\"p90\":" << s.p90;
    out << ",\"p99\":" << s.p99;
    out << ",\"p999\":" << s.p999;
    out << '}';
  }
  out << '}';

  out << '}';
  return out.str();
}

void MetricsRegistry::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [name, counter] : counters_) {
    counter->reset();
  }
  for (auto& [name, gauge] : gauges_) {
    gauge->set(0);
  }
  for (auto& [name, histogram] : histograms_) {
    histogram->reset();
  }
}

}  // namespace bourse
