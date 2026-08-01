#include "bourse/cache/eviction.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <random>
#include <string>

namespace bourse::cache {
namespace {

/// One PRNG per thread. Shared state would be a contention point on the very
/// path eviction is meant to keep cheap, and the quality requirements here are
/// low enough that per-thread streams are fine.
std::mt19937& threadRng() {
  static thread_local std::mt19937 rng{std::random_device{}()};
  return rng;
}

double nextUnitDouble() {
  static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
  return dist(threadRng());
}

constexpr std::uint32_t kMaxFrequency = 255;
constexpr double kLfuLogFactor = 10.0;

}  // namespace

// ---------------------------------------------------------------------------
// LRU
// ---------------------------------------------------------------------------

void LruPolicy::touch(EvictionMetadata& meta, std::int64_t now_ms) const noexcept {
  meta.last_access_ms = now_ms;
}

void LruPolicy::onInsert(EvictionMetadata& meta, std::int64_t now_ms) const noexcept {
  meta.last_access_ms = now_ms;
  meta.frequency = 1;
}

std::size_t LruPolicy::chooseVictim(const std::vector<EvictionMetadata>& sample) const noexcept {
  std::size_t victim = 0;
  std::int64_t oldest = std::numeric_limits<std::int64_t>::max();
  for (std::size_t i = 0; i < sample.size(); ++i) {
    if (sample[i].last_access_ms < oldest) {
      oldest = sample[i].last_access_ms;
      victim = i;
    }
  }
  return victim;
}

// ---------------------------------------------------------------------------
// LFU
// ---------------------------------------------------------------------------

void LfuPolicy::touch(EvictionMetadata& meta, std::int64_t now_ms) const noexcept {
  // 1. Decay first. Without this, anything hot during start-up would stay
  //    resident forever and the cache would never adapt to a shifting working
  //    set. Halving per elapsed interval is cheap and bounded.
  if (decay_interval_ms_ > 0 && meta.last_access_ms > 0 && now_ms > meta.last_access_ms) {
    const std::int64_t elapsed = now_ms - meta.last_access_ms;
    const std::int64_t periods = elapsed / decay_interval_ms_;
    if (periods > 0) {
      meta.frequency = periods >= 32 ? 0 : (meta.frequency >> static_cast<std::uint32_t>(periods));
    }
  }

  // 2. Probabilistic logarithmic increment. A linear counter would saturate
  //    instantly under a hot key and lose all ability to rank; making the
  //    increment less likely as the counter grows keeps the whole 0..255 range
  //    meaningful across many orders of magnitude of access frequency.
  if (meta.frequency < kMaxFrequency) {
    const double probability = 1.0 / (static_cast<double>(meta.frequency) * kLfuLogFactor + 1.0);
    if (nextUnitDouble() < probability) {
      ++meta.frequency;
    }
  }

  meta.last_access_ms = now_ms;
}

void LfuPolicy::onInsert(EvictionMetadata& meta, std::int64_t now_ms) const noexcept {
  meta.last_access_ms = now_ms;
  // Start above zero so a brand-new key is not the instant victim of the very
  // next eviction pass before it has had any chance to prove itself.
  meta.frequency = 5;
}

std::size_t LfuPolicy::chooseVictim(const std::vector<EvictionMetadata>& sample) const noexcept {
  std::size_t victim = 0;
  std::uint32_t lowest = std::numeric_limits<std::uint32_t>::max();
  std::int64_t oldest = std::numeric_limits<std::int64_t>::max();
  for (std::size_t i = 0; i < sample.size(); ++i) {
    const std::uint32_t frequency = sample[i].frequency;
    if (frequency < lowest || (frequency == lowest && sample[i].last_access_ms < oldest)) {
      lowest = frequency;
      oldest = sample[i].last_access_ms;
      victim = i;
    }
  }
  return victim;
}

// ---------------------------------------------------------------------------
// Random
// ---------------------------------------------------------------------------

void RandomPolicy::touch(EvictionMetadata& meta, std::int64_t now_ms) const noexcept {
  meta.last_access_ms = now_ms;
}

void RandomPolicy::onInsert(EvictionMetadata& meta, std::int64_t now_ms) const noexcept {
  meta.last_access_ms = now_ms;
}

std::size_t RandomPolicy::chooseVictim(const std::vector<EvictionMetadata>& sample) const noexcept {
  if (sample.size() <= 1) {
    return 0;
  }
  std::uniform_int_distribution<std::size_t> dist(0, sample.size() - 1);
  return dist(threadRng());
}

// ---------------------------------------------------------------------------
// No eviction
// ---------------------------------------------------------------------------

void NoEvictionPolicy::touch(EvictionMetadata& meta, std::int64_t now_ms) const noexcept {
  meta.last_access_ms = now_ms;
}

void NoEvictionPolicy::onInsert(EvictionMetadata& meta, std::int64_t now_ms) const noexcept {
  meta.last_access_ms = now_ms;
}

std::size_t NoEvictionPolicy::chooseVictim(const std::vector<EvictionMetadata>& /*sample*/) const noexcept {
  return kNoVictim;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<EvictionPolicy> makeEvictionPolicy(std::string_view name) {
  std::string lowered;
  lowered.reserve(name.size());
  for (char c : name) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }

  if (lowered == "allkeys-lru" || lowered == "lru") {
    return std::make_unique<LruPolicy>();
  }
  if (lowered == "allkeys-lfu" || lowered == "lfu") {
    return std::make_unique<LfuPolicy>();
  }
  if (lowered == "allkeys-random" || lowered == "random") {
    return std::make_unique<RandomPolicy>();
  }
  if (lowered == "noeviction" || lowered == "none") {
    return std::make_unique<NoEvictionPolicy>();
  }
  // Unknown names fall back to LRU rather than failing startup: a typo in a
  // config file should not take the server down.
  return std::make_unique<LruPolicy>();
}

}  // namespace bourse::cache
