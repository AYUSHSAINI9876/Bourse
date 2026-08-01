#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bourse/cache/eviction.hpp"
#include "bourse/cache/value.hpp"
#include "bourse/core/common.hpp"
#include "bourse/core/result.hpp"

namespace bourse::cache {

struct KeyspaceOptions {
  /// Must be a power of two. More shards means more write concurrency and more
  /// fixed overhead; 16 is the point where contention stops dominating on a
  /// typical 8-core box.
  std::size_t shard_count = 16;
  /// 0 disables the budget entirely.
  std::size_t max_memory_bytes = 0;
  /// Candidates drawn per eviction decision. Higher is more accurate and
  /// slower; Redis defaults to 5 for the same trade-off.
  std::size_t eviction_sample_size = 5;
  std::string eviction_policy = "allkeys-lru";
};

struct KeyspaceStats {
  std::uint64_t keys = 0;
  std::uint64_t memory_bytes = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t expired = 0;
  std::uint64_t evicted = 0;
  std::uint64_t rejected_writes = 0;
};

/// One stored key.
struct Entry {
  Value value;
  /// Absolute wall-clock deadline in epoch millis. 0 means "never expires".
  /// Absolute rather than relative so it survives a snapshot round-trip.
  std::int64_t expire_at_ms = 0;
  EvictionMetadata eviction;
};

/// Transparent hashing so `get(std::string_view)` does not have to materialise
/// a std::string just to probe the table. Without `is_transparent` every
/// lookup on the hot path would allocate.
struct StringHash {
  using is_transparent = void;
  [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
};

/// Sharded, thread-safe in-memory keyspace with TTLs and approximated eviction.
///
/// **Why a plain mutex per shard and not a shared_mutex.** Reads are not
/// read-only: every GET updates the entry's eviction metadata, so a
/// `shared_lock` would be an outright data race. The options were atomic
/// metadata (extra cost on every access, and still torn across two fields) or
/// exclusive locking with enough shards that it does not matter. Sixteen shards
/// give sixteen-way concurrency, which saturates well past the point the event
/// loop becomes the bottleneck -- so the simpler, obviously-correct option won.
class Keyspace {
 public:
  explicit Keyspace(KeyspaceOptions options = {});
  ~Keyspace();

  Keyspace(const Keyspace&) = delete;
  Keyspace& operator=(const Keyspace&) = delete;

  // -- generic ------------------------------------------------------------
  [[nodiscard]] bool exists(std::string_view key) const;
  bool removeOne(std::string_view key);
  std::size_t remove(const std::vector<std::string>& keys);
  [[nodiscard]] Result<ValueType> typeOf(std::string_view key) const;
  [[nodiscard]] std::vector<std::string> matchingKeys(std::string_view glob) const;
  [[nodiscard]] std::size_t size() const;
  void clear();

  // -- expiry -------------------------------------------------------------
  bool expire(std::string_view key, std::int64_t ttl_ms);
  bool persist(std::string_view key);
  /// -2 when the key does not exist, -1 when it exists without a TTL.
  [[nodiscard]] std::int64_t ttlMillis(std::string_view key) const;

  // -- strings ------------------------------------------------------------
  Status set(std::string_view key, Value value, std::int64_t ttl_ms = 0);
  [[nodiscard]] Result<std::optional<std::string>> get(std::string_view key) const;
  Result<std::int64_t> incrementBy(std::string_view key, std::int64_t delta);
  Result<std::size_t> appendString(std::string_view key, std::string_view suffix);
  [[nodiscard]] Result<std::size_t> stringLength(std::string_view key) const;

  // -- lists --------------------------------------------------------------
  Result<std::size_t> listPush(std::string_view key, const std::vector<std::string>& values, bool front);
  Result<std::optional<std::string>> listPop(std::string_view key, bool front);
  [[nodiscard]] Result<std::vector<std::string>> listRange(std::string_view key, std::int64_t start,
                                                           std::int64_t stop) const;
  [[nodiscard]] Result<std::size_t> listLength(std::string_view key) const;

  // -- hashes -------------------------------------------------------------
  Result<std::size_t> hashSet(std::string_view key, const std::vector<std::pair<std::string, std::string>>& fields);
  [[nodiscard]] Result<std::optional<std::string>> hashGet(std::string_view key, std::string_view field) const;
  [[nodiscard]] Result<std::vector<std::pair<std::string, std::string>>> hashGetAll(std::string_view key) const;
  Result<std::size_t> hashDelete(std::string_view key, const std::vector<std::string>& fields);
  [[nodiscard]] Result<std::size_t> hashLength(std::string_view key) const;

  // -- sets ---------------------------------------------------------------
  Result<std::size_t> setAdd(std::string_view key, const std::vector<std::string>& members);
  Result<std::size_t> setRemove(std::string_view key, const std::vector<std::string>& members);
  [[nodiscard]] Result<bool> setContains(std::string_view key, std::string_view member) const;
  [[nodiscard]] Result<std::vector<std::string>> setMembers(std::string_view key) const;
  [[nodiscard]] Result<std::size_t> setCardinality(std::string_view key) const;

  // -- maintenance --------------------------------------------------------
  /// Samples up to `keys_per_shard` entries per shard and drops the expired
  /// ones. This is the *active* half of expiry; the passive half happens on
  /// access. Neither alone is sufficient: passive-only leaks memory for keys
  /// nobody touches again, active-only cannot keep up with a large keyspace.
  std::size_t activeExpireCycle(std::size_t keys_per_shard = 20);

  /// Evicts until the keyspace is back under `max_memory_bytes`. Returns the
  /// number of keys dropped.
  std::size_t enforceMemoryBudget();

  [[nodiscard]] KeyspaceStats stats() const;
  [[nodiscard]] std::string_view evictionPolicyName() const noexcept { return policy_->name(); }

  /// Visits every live (non-expired) entry. Takes one shard lock at a time, so
  /// a snapshot does not stop the world -- at the cost of not being a single
  /// point-in-time view. Documented in docs/architecture.md.
  void forEachEntry(const std::function<void(const std::string&, const Entry&)>& fn) const;

  /// Bulk load path used by snapshot and AOF replay. Bypasses eviction and
  /// metrics so that restoring does not look like client traffic.
  void restoreEntry(std::string key, Entry entry);

  /// Glob matcher shared with the KEYS command: supports `*`, `?`, `[abc]`,
  /// `[a-z]` and `\` escaping.
  [[nodiscard]] static bool globMatch(std::string_view pattern, std::string_view text);

 private:
  using Map = std::unordered_map<std::string, Entry, StringHash, std::equal_to<>>;

  struct alignas(kCacheLineSize) Shard {
    mutable std::mutex mutex;
    Map map;
    std::size_t memory_bytes = 0;
    mutable std::mt19937_64 rng{std::random_device{}()};
  };

  /// Const-qualified even though it hands back a mutable Shard: the shards are
  /// owned through unique_ptr, whose operator* is const-qualified and does not
  /// propagate constness. The logical constness of a read is enforced by the
  /// callers, not by the type -- a read still has to mutate eviction metadata.
  [[nodiscard]] Shard& shardFor(std::string_view key) const noexcept;

  /// Caller must hold the shard lock. Removes the entry if its TTL has passed
  /// and returns true when it did -- this is the passive expiry path.
  bool reapIfExpired(Shard& shard, Map::iterator it, std::int64_t now_ms) const;

  /// Caller must hold the shard lock. Returns nullptr when absent or expired.
  Entry* lookup(Shard& shard, std::string_view key, std::int64_t now_ms) const;

  /// Caller must hold the shard lock. Finds an existing entry of `required`
  /// type or creates an empty one. Returns kWrongType when a key already holds
  /// a different type, which is the WRONGTYPE error clients see.
  Result<Entry*> obtainForWrite(Shard& shard, std::string_view key, ValueType required, std::int64_t now_ms);

  void accountInsert(Shard& shard, const std::string& key, const Entry& entry) const;
  void accountErase(Shard& shard, const std::string& key, const Entry& entry) const;

  /// Draws up to `n` random keys from a shard by probing random hash buckets.
  /// Caller must hold the shard lock.
  std::vector<Map::iterator> sampleEntries(Shard& shard, std::size_t n) const;

  KeyspaceOptions options_;
  std::unique_ptr<EvictionPolicy> policy_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::size_t shard_mask_ = 0;

  /// Mirrors the sum of every shard's `memory_bytes`. Kept as one atomic so the
  /// budget check on the write path does not have to lock all sixteen shards.
  mutable std::atomic<std::size_t> total_memory_{0};

  mutable std::atomic<std::uint64_t> hits_{0};
  mutable std::atomic<std::uint64_t> misses_{0};
  mutable std::atomic<std::uint64_t> expired_{0};
  mutable std::atomic<std::uint64_t> evicted_{0};
  mutable std::atomic<std::uint64_t> rejected_writes_{0};
};

}  // namespace bourse::cache
