#include "bourse/cache/keyspace.hpp"

#include <algorithm>
#include <bit>
#include <limits>

#include "bourse/core/clock.hpp"

namespace bourse::cache {
namespace {

constexpr std::size_t roundUpPowerOfTwo(std::size_t value) noexcept {
  return value <= 1 ? 1 : std::bit_ceil(value);
}

/// Charged per key on top of the value itself: the hash node, the bucket
/// pointer and the std::string header for the key.
constexpr std::size_t kPerKeyOverhead = 64;

const char* kWrongTypeMessage = "WRONGTYPE Operation against a key holding the wrong kind of value";

std::size_t entryFootprint(const std::string& key, const Entry& entry) {
  return kPerKeyOverhead + key.capacity() + entry.value.approximateBytes();
}

/// Normalises a Redis-style inclusive range with negative indices into a
/// half-open [begin, end) pair clamped to the container.
std::pair<std::size_t, std::size_t> normaliseRange(std::int64_t start, std::int64_t stop, std::size_t size) {
  const auto n = static_cast<std::int64_t>(size);
  if (start < 0) start += n;
  if (stop < 0) stop += n;
  if (start < 0) start = 0;
  if (stop >= n) stop = n - 1;
  if (n == 0 || start > stop || start >= n) {
    return {0, 0};
  }
  return {static_cast<std::size_t>(start), static_cast<std::size_t>(stop) + 1};
}

}  // namespace

Keyspace::Keyspace(KeyspaceOptions options) : options_(std::move(options)) {
  options_.shard_count = roundUpPowerOfTwo(options_.shard_count);
  if (options_.eviction_sample_size == 0) {
    options_.eviction_sample_size = 1;
  }
  policy_ = makeEvictionPolicy(options_.eviction_policy);
  shard_mask_ = options_.shard_count - 1;
  shards_.reserve(options_.shard_count);
  for (std::size_t i = 0; i < options_.shard_count; ++i) {
    shards_.push_back(std::make_unique<Shard>());
  }
}

Keyspace::~Keyspace() = default;

Keyspace::Shard& Keyspace::shardFor(std::string_view key) const noexcept {
  return *shards_[StringHash{}(key) & shard_mask_];
}

void Keyspace::accountInsert(Shard& shard, const std::string& key, const Entry& entry) const {
  const std::size_t bytes = entryFootprint(key, entry);
  shard.memory_bytes += bytes;
  total_memory_.fetch_add(bytes, std::memory_order_relaxed);
}

void Keyspace::accountErase(Shard& shard, const std::string& key, const Entry& entry) const {
  const std::size_t bytes = entryFootprint(key, entry);
  shard.memory_bytes = shard.memory_bytes >= bytes ? shard.memory_bytes - bytes : 0;
  std::size_t previous = total_memory_.load(std::memory_order_relaxed);
  while (!total_memory_.compare_exchange_weak(previous, previous >= bytes ? previous - bytes : 0,
                                              std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
}

bool Keyspace::reapIfExpired(Shard& shard, Map::iterator it, std::int64_t now_ms) const {
  if (it->second.expire_at_ms == 0 || it->second.expire_at_ms > now_ms) {
    return false;
  }
  accountErase(shard, it->first, it->second);
  shard.map.erase(it);
  expired_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

Entry* Keyspace::lookup(Shard& shard, std::string_view key, std::int64_t now_ms) const {
  auto it = shard.map.find(key);
  if (it == shard.map.end()) {
    return nullptr;
  }
  if (reapIfExpired(shard, it, now_ms)) {
    return nullptr;
  }
  return &it->second;
}

Result<Entry*> Keyspace::obtainForWrite(Shard& shard, std::string_view key, ValueType required,
                                        std::int64_t now_ms) {
  auto it = shard.map.find(key);
  if (it != shard.map.end() && reapIfExpired(shard, it, now_ms)) {
    it = shard.map.end();
  }

  if (it != shard.map.end()) {
    if (it->second.value.type() != required) {
      return Status::wrongType(kWrongTypeMessage);
    }
    // The value is about to change size, so drop the old charge; the caller
    // re-accounts once the mutation is done.
    accountErase(shard, it->first, it->second);
    policy_->touch(it->second.eviction, now_ms);
    return &it->second;
  }

  Entry fresh;
  switch (required) {
    case ValueType::kList: fresh.value = Value::makeList({}); break;
    case ValueType::kHash: fresh.value = Value::makeHash({}); break;
    case ValueType::kSet: fresh.value = Value::makeSet({}); break;
    case ValueType::kString: fresh.value = Value::makeString({}); break;
    case ValueType::kInteger: fresh.value = Value::makeInteger(0); break;
    case ValueType::kNone: return Status::invalidArgument("cannot create an entry of type none");
  }
  policy_->onInsert(fresh.eviction, now_ms);
  auto [pos, inserted] = shard.map.emplace(std::string(key), std::move(fresh));
  (void)inserted;
  return &pos->second;
}

// ---------------------------------------------------------------------------
// generic
// ---------------------------------------------------------------------------

bool Keyspace::exists(std::string_view key) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  return lookup(shard, key, now) != nullptr;
}

bool Keyspace::removeOne(std::string_view key) {
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  auto it = shard.map.find(key);
  if (it == shard.map.end()) {
    return false;
  }
  const bool was_live = it->second.expire_at_ms == 0 || it->second.expire_at_ms > nowMillis();
  accountErase(shard, it->first, it->second);
  shard.map.erase(it);
  return was_live;
}

std::size_t Keyspace::remove(const std::vector<std::string>& keys) {
  std::size_t removed = 0;
  for (const std::string& key : keys) {
    if (removeOne(key)) {
      ++removed;
    }
  }
  return removed;
}

Result<ValueType> Keyspace::typeOf(std::string_view key) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  const Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return ValueType::kNone;
  }
  return entry->value.type();
}

std::vector<std::string> Keyspace::matchingKeys(std::string_view glob) const {
  const std::int64_t now = nowMillis();
  std::vector<std::string> result;
  for (const auto& shard_ptr : shards_) {
    Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& [key, entry] : shard.map) {
      if (entry.expire_at_ms != 0 && entry.expire_at_ms <= now) {
        continue;  // logically gone; the expiry cycle will reclaim it
      }
      if (globMatch(glob, key)) {
        result.push_back(key);
      }
    }
  }
  return result;
}

std::size_t Keyspace::size() const {
  std::size_t total = 0;
  for (const auto& shard_ptr : shards_) {
    Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    total += shard.map.size();
  }
  return total;
}

void Keyspace::clear() {
  for (const auto& shard_ptr : shards_) {
    Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.map.clear();
    shard.memory_bytes = 0;
  }
  total_memory_.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// expiry
// ---------------------------------------------------------------------------

bool Keyspace::expire(std::string_view key, std::int64_t ttl_ms) {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return false;
  }
  entry->expire_at_ms = ttl_ms > 0 ? now + ttl_ms : now - 1;
  return true;
}

bool Keyspace::persist(std::string_view key) {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr || entry->expire_at_ms == 0) {
    return false;
  }
  entry->expire_at_ms = 0;
  return true;
}

std::int64_t Keyspace::ttlMillis(std::string_view key) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  const Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return -2;
  }
  if (entry->expire_at_ms == 0) {
    return -1;
  }
  return entry->expire_at_ms - now;
}

// ---------------------------------------------------------------------------
// strings
// ---------------------------------------------------------------------------

Status Keyspace::set(std::string_view key, Value value, std::int64_t ttl_ms) {
  if (options_.max_memory_bytes > 0 && !policy_->evicts() &&
      total_memory_.load(std::memory_order_relaxed) >= options_.max_memory_bytes) {
    rejected_writes_.fetch_add(1, std::memory_order_relaxed);
    return Status(ErrorCode::kOutOfMemory, "OOM command not allowed when used memory > 'maxmemory'");
  }

  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.map.find(key);
    if (it != shard.map.end()) {
      accountErase(shard, it->first, it->second);
      it->second.value = std::move(value);
      it->second.expire_at_ms = ttl_ms > 0 ? now + ttl_ms : 0;
      policy_->touch(it->second.eviction, now);
      accountInsert(shard, it->first, it->second);
    } else {
      Entry entry;
      entry.value = std::move(value);
      entry.expire_at_ms = ttl_ms > 0 ? now + ttl_ms : 0;
      policy_->onInsert(entry.eviction, now);
      auto [pos, inserted] = shard.map.emplace(std::string(key), std::move(entry));
      (void)inserted;
      accountInsert(shard, pos->first, pos->second);
    }
  }

  enforceMemoryBudget();
  return Status::success();
}

Result<std::optional<std::string>> Keyspace::get(std::string_view key) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    return std::optional<std::string>{};
  }
  if (!entry->value.isStringLike()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  policy_->touch(entry->eviction, now);
  hits_.fetch_add(1, std::memory_order_relaxed);
  return std::optional<std::string>{entry->value.toStringValue()};
}

Result<std::int64_t> Keyspace::incrementBy(std::string_view key, std::int64_t delta) {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);

  auto it = shard.map.find(key);
  if (it != shard.map.end() && reapIfExpired(shard, it, now)) {
    it = shard.map.end();
  }

  if (it == shard.map.end()) {
    Entry entry;
    entry.value = Value::makeInteger(delta);
    policy_->onInsert(entry.eviction, now);
    auto [pos, inserted] = shard.map.emplace(std::string(key), std::move(entry));
    (void)inserted;
    accountInsert(shard, pos->first, pos->second);
    return delta;
  }

  if (!it->second.value.isStringLike()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  Result<std::int64_t> current = it->second.value.asInteger();
  if (!current.ok()) {
    return current.status();
  }

  const std::int64_t base = current.value();
  // Signed overflow is UB, so the check has to happen before the addition.
  if ((delta > 0 && base > std::numeric_limits<std::int64_t>::max() - delta) ||
      (delta < 0 && base < std::numeric_limits<std::int64_t>::min() - delta)) {
    return Status::invalidArgument("increment or decrement would overflow");
  }

  accountErase(shard, it->first, it->second);
  const std::int64_t updated = base + delta;
  it->second.value.setInteger(updated);
  policy_->touch(it->second.eviction, now);
  accountInsert(shard, it->first, it->second);
  return updated;
}

Result<std::size_t> Keyspace::appendString(std::string_view key, std::string_view suffix) {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);

  auto it = shard.map.find(key);
  if (it != shard.map.end() && reapIfExpired(shard, it, now)) {
    it = shard.map.end();
  }

  if (it == shard.map.end()) {
    Entry entry;
    entry.value = Value::makeString(std::string(suffix));
    policy_->onInsert(entry.eviction, now);
    auto [pos, inserted] = shard.map.emplace(std::string(key), std::move(entry));
    (void)inserted;
    accountInsert(shard, pos->first, pos->second);
    return suffix.size();
  }

  if (!it->second.value.isStringLike()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  accountErase(shard, it->first, it->second);
  std::string combined = it->second.value.toStringValue();
  combined.append(suffix);
  const std::size_t length = combined.size();
  it->second.value.setString(std::move(combined));
  policy_->touch(it->second.eviction, now);
  accountInsert(shard, it->first, it->second);
  return length;
}

Result<std::size_t> Keyspace::stringLength(std::string_view key) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return std::size_t{0};
  }
  if (!entry->value.isStringLike()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  return entry->value.toStringValue().size();
}

// ---------------------------------------------------------------------------
// lists
// ---------------------------------------------------------------------------

Result<std::size_t> Keyspace::listPush(std::string_view key, const std::vector<std::string>& values, bool front) {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);

  Result<Entry*> slot = obtainForWrite(shard, key, ValueType::kList, now);
  if (!slot.ok()) {
    return slot.status();
  }
  Entry* entry = slot.value();
  ListValue& list = entry->value.list();
  for (const std::string& value : values) {
    if (front) {
      list.push_front(value);
    } else {
      list.push_back(value);
    }
  }
  const std::size_t length = list.size();
  accountInsert(shard, shard.map.find(key)->first, *entry);
  return length;
}

Result<std::optional<std::string>> Keyspace::listPop(std::string_view key, bool front) {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);

  auto it = shard.map.find(key);
  if (it != shard.map.end() && reapIfExpired(shard, it, now)) {
    it = shard.map.end();
  }
  if (it == shard.map.end()) {
    return std::optional<std::string>{};
  }
  if (!it->second.value.isList()) {
    return Status::wrongType(kWrongTypeMessage);
  }

  ListValue& list = it->second.value.list();
  if (list.empty()) {
    return std::optional<std::string>{};
  }

  accountErase(shard, it->first, it->second);
  std::string popped;
  if (front) {
    popped = std::move(list.front());
    list.pop_front();
  } else {
    popped = std::move(list.back());
    list.pop_back();
  }
  policy_->touch(it->second.eviction, now);

  // Redis semantics: a list that becomes empty stops existing.
  if (list.empty()) {
    shard.map.erase(it);
  } else {
    accountInsert(shard, it->first, it->second);
  }
  return std::optional<std::string>{std::move(popped)};
}

Result<std::vector<std::string>> Keyspace::listRange(std::string_view key, std::int64_t start,
                                                     std::int64_t stop) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return std::vector<std::string>{};
  }
  if (!entry->value.isList()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  policy_->touch(entry->eviction, now);

  const ListValue& list = entry->value.list();
  const auto [begin, end] = normaliseRange(start, stop, list.size());
  std::vector<std::string> out;
  out.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    out.push_back(list[i]);
  }
  return out;
}

Result<std::size_t> Keyspace::listLength(std::string_view key) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return std::size_t{0};
  }
  if (!entry->value.isList()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  return entry->value.list().size();
}

// ---------------------------------------------------------------------------
// hashes
// ---------------------------------------------------------------------------

Result<std::size_t> Keyspace::hashSet(std::string_view key,
                                      const std::vector<std::pair<std::string, std::string>>& fields) {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);

  Result<Entry*> slot = obtainForWrite(shard, key, ValueType::kHash, now);
  if (!slot.ok()) {
    return slot.status();
  }
  Entry* entry = slot.value();
  HashValue& hash = entry->value.hash();
  std::size_t added = 0;
  for (const auto& [field, value] : fields) {
    if (hash.insert_or_assign(field, value).second) {
      ++added;
    }
  }
  accountInsert(shard, shard.map.find(key)->first, *entry);
  return added;
}

Result<std::optional<std::string>> Keyspace::hashGet(std::string_view key, std::string_view field) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return std::optional<std::string>{};
  }
  if (!entry->value.isHash()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  policy_->touch(entry->eviction, now);
  const HashValue& hash = entry->value.hash();
  auto it = hash.find(std::string(field));
  if (it == hash.end()) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>{it->second};
}

Result<std::vector<std::pair<std::string, std::string>>> Keyspace::hashGetAll(std::string_view key) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return std::vector<std::pair<std::string, std::string>>{};
  }
  if (!entry->value.isHash()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  policy_->touch(entry->eviction, now);
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(entry->value.hash().size());
  for (const auto& [field, value] : entry->value.hash()) {
    out.emplace_back(field, value);
  }
  return out;
}

Result<std::size_t> Keyspace::hashDelete(std::string_view key, const std::vector<std::string>& fields) {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);

  auto it = shard.map.find(key);
  if (it != shard.map.end() && reapIfExpired(shard, it, now)) {
    it = shard.map.end();
  }
  if (it == shard.map.end()) {
    return std::size_t{0};
  }
  if (!it->second.value.isHash()) {
    return Status::wrongType(kWrongTypeMessage);
  }

  accountErase(shard, it->first, it->second);
  HashValue& hash = it->second.value.hash();
  std::size_t removed = 0;
  for (const std::string& field : fields) {
    removed += hash.erase(field);
  }
  if (hash.empty()) {
    shard.map.erase(it);
  } else {
    accountInsert(shard, it->first, it->second);
  }
  return removed;
}

Result<std::size_t> Keyspace::hashLength(std::string_view key) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return std::size_t{0};
  }
  if (!entry->value.isHash()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  return entry->value.hash().size();
}

// ---------------------------------------------------------------------------
// sets
// ---------------------------------------------------------------------------

Result<std::size_t> Keyspace::setAdd(std::string_view key, const std::vector<std::string>& members) {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);

  Result<Entry*> slot = obtainForWrite(shard, key, ValueType::kSet, now);
  if (!slot.ok()) {
    return slot.status();
  }
  Entry* entry = slot.value();
  SetValue& set = entry->value.set();
  std::size_t added = 0;
  for (const std::string& member : members) {
    if (set.insert(member).second) {
      ++added;
    }
  }
  accountInsert(shard, shard.map.find(key)->first, *entry);
  return added;
}

Result<std::size_t> Keyspace::setRemove(std::string_view key, const std::vector<std::string>& members) {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);

  auto it = shard.map.find(key);
  if (it != shard.map.end() && reapIfExpired(shard, it, now)) {
    it = shard.map.end();
  }
  if (it == shard.map.end()) {
    return std::size_t{0};
  }
  if (!it->second.value.isSet()) {
    return Status::wrongType(kWrongTypeMessage);
  }

  accountErase(shard, it->first, it->second);
  SetValue& set = it->second.value.set();
  std::size_t removed = 0;
  for (const std::string& member : members) {
    removed += set.erase(member);
  }
  if (set.empty()) {
    shard.map.erase(it);
  } else {
    accountInsert(shard, it->first, it->second);
  }
  return removed;
}

Result<bool> Keyspace::setContains(std::string_view key, std::string_view member) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return false;
  }
  if (!entry->value.isSet()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  policy_->touch(entry->eviction, now);
  return entry->value.set().count(std::string(member)) > 0;
}

Result<std::vector<std::string>> Keyspace::setMembers(std::string_view key) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return std::vector<std::string>{};
  }
  if (!entry->value.isSet()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  policy_->touch(entry->eviction, now);
  std::vector<std::string> out;
  out.reserve(entry->value.set().size());
  for (const std::string& member : entry->value.set()) {
    out.push_back(member);
  }
  return out;
}

Result<std::size_t> Keyspace::setCardinality(std::string_view key) const {
  const std::int64_t now = nowMillis();
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  Entry* entry = lookup(shard, key, now);
  if (entry == nullptr) {
    return std::size_t{0};
  }
  if (!entry->value.isSet()) {
    return Status::wrongType(kWrongTypeMessage);
  }
  return entry->value.set().size();
}

// ---------------------------------------------------------------------------
// maintenance
// ---------------------------------------------------------------------------

std::vector<Keyspace::Map::iterator> Keyspace::sampleEntries(Shard& shard, std::size_t n) const {
  std::vector<Map::iterator> sample;
  if (shard.map.empty() || n == 0) {
    return sample;
  }
  const std::size_t buckets = shard.map.bucket_count();
  if (buckets == 0) {
    return sample;
  }
  sample.reserve(n);

  // Probing random buckets is O(1) per candidate. The obvious alternative --
  // std::advance from begin() by a random offset -- is O(size) and would make
  // eviction quadratic on a large keyspace.
  std::uniform_int_distribution<std::size_t> dist(0, buckets - 1);
  const std::size_t max_probes = n * 8 + 16;

  for (std::size_t probe = 0; probe < max_probes && sample.size() < n; ++probe) {
    const std::size_t origin = dist(shard.rng);
    for (std::size_t offset = 0; offset < buckets; ++offset) {
      const std::size_t bucket = (origin + offset) % buckets;
      auto local = shard.map.begin(bucket);
      if (local == shard.map.end(bucket)) {
        continue;
      }
      auto it = shard.map.find(local->first);
      const bool already_taken =
          std::any_of(sample.begin(), sample.end(), [&](const Map::iterator& taken) { return taken == it; });
      if (!already_taken) {
        sample.push_back(it);
      }
      break;
    }
  }
  return sample;
}

std::size_t Keyspace::activeExpireCycle(std::size_t keys_per_shard) {
  const std::int64_t now = nowMillis();
  std::size_t reclaimed = 0;

  for (const auto& shard_ptr : shards_) {
    Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    std::vector<Map::iterator> sample = sampleEntries(shard, keys_per_shard);
    for (Map::iterator it : sample) {
      if (it == shard.map.end()) {
        continue;
      }
      if (it->second.expire_at_ms != 0 && it->second.expire_at_ms <= now) {
        accountErase(shard, it->first, it->second);
        shard.map.erase(it);
        expired_.fetch_add(1, std::memory_order_relaxed);
        ++reclaimed;
      }
    }
  }
  return reclaimed;
}

std::size_t Keyspace::enforceMemoryBudget() {
  if (options_.max_memory_bytes == 0 || !policy_->evicts()) {
    return 0;
  }

  std::size_t evicted = 0;
  // Bounded so a pathological configuration cannot spin here forever holding
  // up the caller; the next write will continue making progress.
  constexpr std::size_t kMaxEvictionsPerPass = 10'000;

  while (total_memory_.load(std::memory_order_relaxed) > options_.max_memory_bytes &&
         evicted < kMaxEvictionsPerPass) {
    // Always attack the largest shard: evicting from a small shard would not
    // move the global figure and the loop would never terminate.
    //
    // The candidate's size is copied out while its own lock is held. Comparing
    // `fattest->memory_bytes` directly would read another shard's field with no
    // lock -- a data race that ThreadSanitizer flags, and a real one on a
    // weakly-ordered target.
    Shard* fattest = nullptr;
    std::size_t fattest_bytes = 0;
    for (const auto& shard_ptr : shards_) {
      Shard& shard = *shard_ptr;
      std::size_t bytes = 0;
      {
        std::lock_guard<std::mutex> lock(shard.mutex);
        if (shard.map.empty()) {
          continue;
        }
        bytes = shard.memory_bytes;
      }
      if (fattest == nullptr || bytes > fattest_bytes) {
        fattest = &shard;
        fattest_bytes = bytes;
      }
    }
    if (fattest == nullptr) {
      break;  // nothing left to evict
    }

    std::lock_guard<std::mutex> lock(fattest->mutex);
    std::vector<Map::iterator> candidates = sampleEntries(*fattest, options_.eviction_sample_size);
    if (candidates.empty()) {
      break;
    }

    std::vector<EvictionMetadata> metadata;
    metadata.reserve(candidates.size());
    for (Map::iterator it : candidates) {
      metadata.push_back(it->second.eviction);
    }

    const std::size_t victim = policy_->chooseVictim(metadata);
    if (victim == EvictionPolicy::kNoVictim || victim >= candidates.size()) {
      break;
    }

    Map::iterator doomed = candidates[victim];
    accountErase(*fattest, doomed->first, doomed->second);
    fattest->map.erase(doomed);
    evicted_.fetch_add(1, std::memory_order_relaxed);
    ++evicted;
  }
  return evicted;
}

KeyspaceStats Keyspace::stats() const {
  KeyspaceStats out;
  out.keys = size();
  out.memory_bytes = total_memory_.load(std::memory_order_relaxed);
  out.hits = hits_.load(std::memory_order_relaxed);
  out.misses = misses_.load(std::memory_order_relaxed);
  out.expired = expired_.load(std::memory_order_relaxed);
  out.evicted = evicted_.load(std::memory_order_relaxed);
  out.rejected_writes = rejected_writes_.load(std::memory_order_relaxed);
  return out;
}

void Keyspace::forEachEntry(const std::function<void(const std::string&, const Entry&)>& fn) const {
  const std::int64_t now = nowMillis();
  for (const auto& shard_ptr : shards_) {
    Shard& shard = *shard_ptr;
    std::lock_guard<std::mutex> lock(shard.mutex);
    for (const auto& [key, entry] : shard.map) {
      if (entry.expire_at_ms != 0 && entry.expire_at_ms <= now) {
        continue;
      }
      fn(key, entry);
    }
  }
}

void Keyspace::restoreEntry(std::string key, Entry entry) {
  Shard& shard = shardFor(key);
  std::lock_guard<std::mutex> lock(shard.mutex);
  auto it = shard.map.find(key);
  if (it != shard.map.end()) {
    accountErase(shard, it->first, it->second);
    shard.map.erase(it);
  }
  auto [pos, inserted] = shard.map.emplace(std::move(key), std::move(entry));
  (void)inserted;
  accountInsert(shard, pos->first, pos->second);
}

// ---------------------------------------------------------------------------
// glob matching
// ---------------------------------------------------------------------------

bool Keyspace::globMatch(std::string_view pattern, std::string_view text) {
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t star_p = std::string_view::npos;
  std::size_t star_t = 0;

  // Iterative backtracking rather than recursion: a pattern of many '*'
  // characters against a long key would blow the stack in the recursive form,
  // and this variant is O(n*m) worst case with O(1) space.
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
      continue;
    }
    if (p < pattern.size() && pattern[p] == '\\' && p + 1 < pattern.size() && pattern[p + 1] == text[t]) {
      p += 2;
      ++t;
      continue;
    }
    if (p < pattern.size() && pattern[p] == '[') {
      const std::size_t close = pattern.find(']', p + 1);
      if (close != std::string_view::npos) {
        bool negate = false;
        std::size_t cursor = p + 1;
        if (cursor < close && (pattern[cursor] == '^' || pattern[cursor] == '!')) {
          negate = true;
          ++cursor;
        }
        bool matched = false;
        while (cursor < close) {
          if (cursor + 2 < close && pattern[cursor + 1] == '-') {
            if (text[t] >= pattern[cursor] && text[t] <= pattern[cursor + 2]) {
              matched = true;
            }
            cursor += 3;
          } else {
            if (pattern[cursor] == text[t]) {
              matched = true;
            }
            ++cursor;
          }
        }
        if (matched != negate) {
          p = close + 1;
          ++t;
          continue;
        }
      }
    }
    if (p < pattern.size() && pattern[p] == '*') {
      star_p = p++;
      star_t = t;
      continue;
    }
    if (star_p != std::string_view::npos) {
      p = star_p + 1;
      t = ++star_t;
      continue;
    }
    return false;
  }

  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

}  // namespace bourse::cache
