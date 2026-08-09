#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bourse::cache {

/// Per-key bookkeeping the eviction policies read.
///
/// Kept deliberately small (16 bytes) because it is embedded in every entry in
/// the keyspace; anything larger measurably increases the working set.
struct EvictionMetadata {
  std::int64_t last_access_ms = 0;
  std::uint32_t frequency = 0;  ///< LFU counter, logarithmically incremented
  std::uint32_t padding = 0;
};

/// Strategy interface for choosing what to drop when the keyspace is over its
/// memory budget.
///
/// The important design decision is what this interface does *not* do: it never
/// sees the whole keyspace. Exact LRU would require an intrusive list touched
/// on every read, turning every GET into a write plus a pointer chase through
/// cold memory. Instead the keyspace draws a small random sample and asks the
/// policy to rank only that -- the same approximation Redis makes, and the
/// reason `maxmemory-samples` exists there. Sample size trades eviction
/// accuracy against CPU, and is configurable.
class EvictionPolicy {
 public:
  virtual ~EvictionPolicy() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// Called whenever an existing key is read or written.
  virtual void touch(EvictionMetadata& meta, std::int64_t now_ms) const noexcept = 0;

  /// Called when a key is first created.
  virtual void onInsert(EvictionMetadata& meta, std::int64_t now_ms) const noexcept = 0;

  /// Ranks a sample and returns the index of the entry to evict. The sample is
  /// never empty. Returning `kNoVictim` means "evict nothing", which is how
  /// the no-eviction policy refuses writes instead of dropping data.
  [[nodiscard]] virtual std::size_t chooseVictim(
      const std::vector<EvictionMetadata>& sample) const noexcept = 0;

  /// True when the policy is willing to discard data at all.
  [[nodiscard]] virtual bool evicts() const noexcept { return true; }

  static constexpr std::size_t kNoVictim = static_cast<std::size_t>(-1);
};

/// Approximated LRU: evicts the sampled key with the oldest access timestamp.
class LruPolicy final : public EvictionPolicy {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "allkeys-lru"; }

  void touch(EvictionMetadata& meta, std::int64_t now_ms) const noexcept override;
  void onInsert(EvictionMetadata& meta, std::int64_t now_ms) const noexcept override;
  [[nodiscard]] std::size_t chooseVictim(const std::vector<EvictionMetadata>& sample) const noexcept override;
};

/// Approximated LFU with logarithmic counter growth and time decay.
///
/// A plain frequency counter is wrong for a cache: a key hammered during
/// start-up would stay resident forever. The counter therefore increments
/// probabilistically (so it saturates rather than growing without bound) and
/// halves on a decay interval, which is Redis's LFU design.
class LfuPolicy final : public EvictionPolicy {
 public:
  explicit LfuPolicy(std::int64_t decay_interval_ms = 60'000) : decay_interval_ms_(decay_interval_ms) {}

  [[nodiscard]] std::string_view name() const noexcept override { return "allkeys-lfu"; }

  void touch(EvictionMetadata& meta, std::int64_t now_ms) const noexcept override;
  void onInsert(EvictionMetadata& meta, std::int64_t now_ms) const noexcept override;
  [[nodiscard]] std::size_t chooseVictim(const std::vector<EvictionMetadata>& sample) const noexcept override;

 private:
  std::int64_t decay_interval_ms_;
};

/// Uniformly random victim. Cheapest possible policy; useful as a baseline in
/// the benchmark suite to show what LRU/LFU actually buy.
class RandomPolicy final : public EvictionPolicy {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "allkeys-random"; }

  void touch(EvictionMetadata& meta, std::int64_t now_ms) const noexcept override;
  void onInsert(EvictionMetadata& meta, std::int64_t now_ms) const noexcept override;
  [[nodiscard]] std::size_t chooseVictim(const std::vector<EvictionMetadata>& sample) const noexcept override;
};

/// Refuses to evict. Writes that would exceed the memory budget fail with an
/// OOM error instead, which is the correct behaviour when the store is being
/// used as a system of record rather than as a cache.
class NoEvictionPolicy final : public EvictionPolicy {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "noeviction"; }

  void touch(EvictionMetadata& meta, std::int64_t now_ms) const noexcept override;
  void onInsert(EvictionMetadata& meta, std::int64_t now_ms) const noexcept override;
  [[nodiscard]] std::size_t chooseVictim(const std::vector<EvictionMetadata>& sample) const noexcept override;

  [[nodiscard]] bool evicts() const noexcept override { return false; }
};

/// Factory. Accepts the same policy names Redis uses so existing tooling and
/// muscle memory transfer.
[[nodiscard]] std::unique_ptr<EvictionPolicy> makeEvictionPolicy(std::string_view name);

}  // namespace bourse::cache
