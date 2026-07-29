#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "bourse/cache/eviction.hpp"
#include "bourse/cache/keyspace.hpp"

using namespace bourse;
using namespace bourse::cache;

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

TEST(Value, TracksItsAlternative) {
  EXPECT_EQ(Value{}.type(), ValueType::kNone);
  EXPECT_EQ(Value::makeString("x").type(), ValueType::kString);
  EXPECT_EQ(Value::makeInteger(1).type(), ValueType::kInteger);
  EXPECT_EQ(Value::makeList(ListValue{"a"}).type(), ValueType::kList);
}

TEST(Value, IntegerEncodingIsInvisibleToClients) {
  // Both encodings must report themselves as "string" over the wire, exactly
  // as Redis does -- the int encoding is an internal optimisation only.
  EXPECT_STREQ(toString(ValueType::kInteger), "string");
  EXPECT_STREQ(toString(ValueType::kString), "string");

  const Value integer = Value::makeInteger(42);
  EXPECT_TRUE(integer.isStringLike());
  EXPECT_EQ(integer.toStringValue(), "42");
}

TEST(Value, ParsesIntegersStrictly) {
  EXPECT_TRUE(parseInteger("123").ok());
  EXPECT_EQ(parseInteger("123").value(), 123);
  EXPECT_EQ(parseInteger("-45").value(), -45);

  // from_chars stops at the first invalid character and reports success, so
  // rejecting trailing junk has to be explicit.
  EXPECT_FALSE(parseInteger("12abc").ok());
  EXPECT_FALSE(parseInteger("").ok());
  EXPECT_FALSE(parseInteger(" 12").ok());
  EXPECT_FALSE(parseInteger("99999999999999999999999").ok());
}

// ---------------------------------------------------------------------------
// Glob matching
// ---------------------------------------------------------------------------

TEST(GlobMatch, HandlesWildcardsAndClasses) {
  EXPECT_TRUE(Keyspace::globMatch("*", "anything"));
  EXPECT_TRUE(Keyspace::globMatch("user:*", "user:42"));
  EXPECT_FALSE(Keyspace::globMatch("user:*", "order:42"));
  EXPECT_TRUE(Keyspace::globMatch("h?llo", "hello"));
  EXPECT_FALSE(Keyspace::globMatch("h?llo", "heello"));
  EXPECT_TRUE(Keyspace::globMatch("h[ae]llo", "hallo"));
  EXPECT_FALSE(Keyspace::globMatch("h[ae]llo", "hillo"));
  EXPECT_TRUE(Keyspace::globMatch("key[0-9]", "key7"));
  EXPECT_TRUE(Keyspace::globMatch("*:*:*", "a:b:c"));
  EXPECT_TRUE(Keyspace::globMatch("exact", "exact"));
  EXPECT_FALSE(Keyspace::globMatch("exact", "exactly"));
}

TEST(GlobMatch, DoesNotBlowUpOnPathologicalPatterns) {
  // The iterative backtracking form exists so this cannot recurse to death.
  const std::string pattern(64, '*');
  const std::string text(2000, 'a');
  EXPECT_TRUE(Keyspace::globMatch(pattern, text));
}

// ---------------------------------------------------------------------------
// Keyspace basics
// ---------------------------------------------------------------------------

TEST(Keyspace, SetGetDelete) {
  Keyspace keyspace;
  ASSERT_TRUE(keyspace.set("k", Value::makeString("v")).ok());

  auto value = keyspace.get("k");
  ASSERT_TRUE(value.ok());
  ASSERT_TRUE(value.value().has_value());
  EXPECT_EQ(*value.value(), "v");

  EXPECT_TRUE(keyspace.exists("k"));
  EXPECT_TRUE(keyspace.removeOne("k"));
  EXPECT_FALSE(keyspace.exists("k"));

  auto missing = keyspace.get("k");
  ASSERT_TRUE(missing.ok());
  EXPECT_FALSE(missing.value().has_value());
}

TEST(Keyspace, WrongTypeIsReportedNotIgnored) {
  Keyspace keyspace;
  ASSERT_TRUE(keyspace.listPush("mylist", {"a"}, false).ok());

  auto as_string = keyspace.get("mylist");
  ASSERT_FALSE(as_string.ok());
  EXPECT_EQ(as_string.status().code(), ErrorCode::kWrongType);

  auto as_hash = keyspace.hashGet("mylist", "field");
  EXPECT_FALSE(as_hash.ok());
}

TEST(Keyspace, IncrementCreatesAndAccumulates) {
  Keyspace keyspace;
  EXPECT_EQ(keyspace.incrementBy("counter", 1).value(), 1);
  EXPECT_EQ(keyspace.incrementBy("counter", 10).value(), 11);
  EXPECT_EQ(keyspace.incrementBy("counter", -6).value(), 5);

  ASSERT_TRUE(keyspace.set("text", Value::makeString("abc")).ok());
  EXPECT_FALSE(keyspace.incrementBy("text", 1).ok());
}

TEST(Keyspace, IncrementRefusesToOverflow) {
  Keyspace keyspace;
  ASSERT_TRUE(keyspace.set("big", Value::makeInteger(std::numeric_limits<std::int64_t>::max())).ok());
  // Signed overflow is undefined behaviour, so this must be rejected before
  // the addition rather than detected afterwards.
  EXPECT_FALSE(keyspace.incrementBy("big", 1).ok());

  ASSERT_TRUE(keyspace.set("small", Value::makeInteger(std::numeric_limits<std::int64_t>::min())).ok());
  EXPECT_FALSE(keyspace.incrementBy("small", -1).ok());
}

// ---------------------------------------------------------------------------
// TTL
// ---------------------------------------------------------------------------

TEST(Keyspace, TtlReportsMinusOneAndMinusTwo) {
  Keyspace keyspace;
  EXPECT_EQ(keyspace.ttlMillis("absent"), -2);

  ASSERT_TRUE(keyspace.set("k", Value::makeString("v")).ok());
  EXPECT_EQ(keyspace.ttlMillis("k"), -1);

  EXPECT_TRUE(keyspace.expire("k", 10000));
  EXPECT_GT(keyspace.ttlMillis("k"), 9000);

  EXPECT_TRUE(keyspace.persist("k"));
  EXPECT_EQ(keyspace.ttlMillis("k"), -1);
}

TEST(Keyspace, PassiveExpiryHidesTheKeyOnAccess) {
  Keyspace keyspace;
  ASSERT_TRUE(keyspace.set("doomed", Value::makeString("v"), 20).ok());
  EXPECT_TRUE(keyspace.exists("doomed"));

  std::this_thread::sleep_for(std::chrono::milliseconds(40));

  // Nothing has swept yet; the key must disappear on the read path alone.
  EXPECT_FALSE(keyspace.exists("doomed"));
  auto value = keyspace.get("doomed");
  ASSERT_TRUE(value.ok());
  EXPECT_FALSE(value.value().has_value());
}

TEST(Keyspace, ActiveExpiryReclaimsUntouchedKeys) {
  // Passive expiry alone leaks memory for keys nobody reads again. The active
  // cycle is what bounds that, so it gets its own test.
  Keyspace keyspace;
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(keyspace.set("k" + std::to_string(i), Value::makeString("v"), 10).ok());
  }
  const std::size_t before = keyspace.size();
  EXPECT_EQ(before, 200u);

  std::this_thread::sleep_for(std::chrono::milliseconds(40));

  std::size_t reclaimed = 0;
  for (int pass = 0; pass < 40 && keyspace.size() > 0; ++pass) {
    reclaimed += keyspace.activeExpireCycle(40);
  }
  EXPECT_GT(reclaimed, 0u);
  EXPECT_LT(keyspace.size(), before);
}

// ---------------------------------------------------------------------------
// Collections
// ---------------------------------------------------------------------------

TEST(Keyspace, ListPushPopRange) {
  Keyspace keyspace;
  EXPECT_EQ(keyspace.listPush("q", {"a", "b", "c"}, false).value(), 3u);
  EXPECT_EQ(keyspace.listPush("q", {"z"}, true).value(), 4u);

  auto range = keyspace.listRange("q", 0, -1);
  ASSERT_TRUE(range.ok());
  EXPECT_EQ(range.value(), (std::vector<std::string>{"z", "a", "b", "c"}));

  EXPECT_EQ(*keyspace.listPop("q", true).value(), "z");
  EXPECT_EQ(*keyspace.listPop("q", false).value(), "c");
  EXPECT_EQ(keyspace.listLength("q").value(), 2u);
}

TEST(Keyspace, EmptiedListStopsExisting) {
  Keyspace keyspace;
  ASSERT_TRUE(keyspace.listPush("q", {"only"}, false).ok());
  EXPECT_EQ(*keyspace.listPop("q", true).value(), "only");
  EXPECT_FALSE(keyspace.exists("q")) << "an emptied list must be removed, as in Redis";
}

TEST(Keyspace, ListRangeHandlesNegativeAndOutOfBoundsIndices) {
  Keyspace keyspace;
  ASSERT_TRUE(keyspace.listPush("q", {"a", "b", "c", "d"}, false).ok());

  EXPECT_EQ(keyspace.listRange("q", 1, 2).value(), (std::vector<std::string>{"b", "c"}));
  EXPECT_EQ(keyspace.listRange("q", -2, -1).value(), (std::vector<std::string>{"c", "d"}));
  EXPECT_EQ(keyspace.listRange("q", 0, 100).value(), (std::vector<std::string>{"a", "b", "c", "d"}));
  EXPECT_TRUE(keyspace.listRange("q", 3, 1).value().empty());
  EXPECT_TRUE(keyspace.listRange("absent", 0, -1).value().empty());
}

TEST(Keyspace, HashOperations) {
  Keyspace keyspace;
  EXPECT_EQ(keyspace.hashSet("h", {{"a", "1"}, {"b", "2"}}).value(), 2u);
  EXPECT_EQ(keyspace.hashSet("h", {{"a", "9"}}).value(), 0u) << "overwrite is not an insert";
  EXPECT_EQ(*keyspace.hashGet("h", "a").value(), "9");
  EXPECT_EQ(keyspace.hashLength("h").value(), 2u);
  EXPECT_EQ(keyspace.hashDelete("h", {"a", "missing"}).value(), 1u);
  EXPECT_EQ(keyspace.hashLength("h").value(), 1u);
}

TEST(Keyspace, SetOperations) {
  Keyspace keyspace;
  EXPECT_EQ(keyspace.setAdd("s", {"a", "b", "a"}).value(), 2u);
  EXPECT_TRUE(keyspace.setContains("s", "a").value());
  EXPECT_FALSE(keyspace.setContains("s", "zzz").value());
  EXPECT_EQ(keyspace.setCardinality("s").value(), 2u);

  auto members = keyspace.setMembers("s").value();
  std::sort(members.begin(), members.end());
  EXPECT_EQ(members, (std::vector<std::string>{"a", "b"}));

  EXPECT_EQ(keyspace.setRemove("s", {"a"}).value(), 1u);
  EXPECT_EQ(keyspace.setCardinality("s").value(), 1u);
}

// ---------------------------------------------------------------------------
// Eviction
// ---------------------------------------------------------------------------

TEST(EvictionPolicy, FactoryAcceptsRedisNames) {
  EXPECT_EQ(makeEvictionPolicy("allkeys-lru")->name(), "allkeys-lru");
  EXPECT_EQ(makeEvictionPolicy("allkeys-lfu")->name(), "allkeys-lfu");
  EXPECT_EQ(makeEvictionPolicy("allkeys-random")->name(), "allkeys-random");
  EXPECT_EQ(makeEvictionPolicy("noeviction")->name(), "noeviction");
  // A typo in a config file must not take the server down.
  EXPECT_EQ(makeEvictionPolicy("nonsense")->name(), "allkeys-lru");
}

TEST(EvictionPolicy, LruPicksTheOldestSample) {
  LruPolicy policy;
  std::vector<EvictionMetadata> sample(3);
  sample[0].last_access_ms = 500;
  sample[1].last_access_ms = 100;  // oldest
  sample[2].last_access_ms = 900;
  EXPECT_EQ(policy.chooseVictim(sample), 1u);
}

TEST(EvictionPolicy, LfuPicksTheLeastFrequentSample) {
  LfuPolicy policy;
  std::vector<EvictionMetadata> sample(3);
  sample[0].frequency = 10;
  sample[1].frequency = 200;
  sample[2].frequency = 3;  // coldest
  EXPECT_EQ(policy.chooseVictim(sample), 2u);
}

TEST(EvictionPolicy, LfuDecaysSoStartupHotKeysDoNotPinForever) {
  LfuPolicy policy(/*decay_interval_ms=*/10);
  EvictionMetadata meta;
  policy.onInsert(meta, 0);
  const std::uint32_t initial = meta.frequency;
  ASSERT_GT(initial, 0u);

  // Touch far in the future: many decay periods have elapsed.
  policy.touch(meta, 10'000);
  EXPECT_LT(meta.frequency, initial + 2) << "counter failed to decay across idle periods";
}

TEST(EvictionPolicy, NoEvictionRefusesToChoose) {
  NoEvictionPolicy policy;
  std::vector<EvictionMetadata> sample(3);
  EXPECT_EQ(policy.chooseVictim(sample), EvictionPolicy::kNoVictim);
  EXPECT_FALSE(policy.evicts());
}

TEST(Keyspace, EvictsBackUnderTheMemoryBudget) {
  KeyspaceOptions options;
  options.shard_count = 4;
  options.max_memory_bytes = 64 * 1024;
  options.eviction_policy = "allkeys-lru";
  Keyspace keyspace(options);

  const std::string payload(256, 'x');
  for (int i = 0; i < 2000; ++i) {
    (void)keyspace.set("key" + std::to_string(i), Value::makeString(payload));
  }
  keyspace.enforceMemoryBudget();

  const KeyspaceStats stats = keyspace.stats();
  EXPECT_GT(stats.evicted, 0u) << "nothing was evicted despite exceeding the budget";
  EXPECT_LE(stats.memory_bytes, options.max_memory_bytes * 2)
      << "memory stayed far above the budget after enforcement";
  EXPECT_GT(stats.keys, 0u) << "eviction should not empty the keyspace entirely";
}

TEST(Keyspace, NoEvictionRejectsWritesInsteadOfDroppingData) {
  KeyspaceOptions options;
  options.shard_count = 2;
  options.max_memory_bytes = 16 * 1024;
  options.eviction_policy = "noeviction";
  Keyspace keyspace(options);

  const std::string payload(512, 'x');
  bool saw_rejection = false;
  for (int i = 0; i < 500; ++i) {
    const Status status = keyspace.set("key" + std::to_string(i), Value::makeString(payload));
    if (!status.ok()) {
      EXPECT_EQ(status.code(), ErrorCode::kOutOfMemory);
      saw_rejection = true;
      break;
    }
  }
  EXPECT_TRUE(saw_rejection) << "noeviction must refuse writes once over budget";
  EXPECT_GT(keyspace.stats().rejected_writes, 0u);
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

TEST(Keyspace, ConcurrentIncrementsLoseNothing) {
  // The sharded-mutex design's whole justification is that it stays correct
  // under concurrency. Eight threads incrementing eight counters must produce
  // exactly the arithmetic total.
  Keyspace keyspace;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 2000;

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&keyspace] {
      for (int i = 0; i < kPerThread; ++i) {
        (void)keyspace.incrementBy("counter:" + std::to_string(i % 8), 1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  std::int64_t total = 0;
  for (int i = 0; i < 8; ++i) {
    auto value = keyspace.get("counter:" + std::to_string(i));
    ASSERT_TRUE(value.ok());
    ASSERT_TRUE(value.value().has_value());
    total += parseInteger(*value.value()).value();
  }
  EXPECT_EQ(total, static_cast<std::int64_t>(kThreads) * kPerThread);
}

TEST(Keyspace, ConcurrentMixedWorkloadDoesNotCorrupt) {
  Keyspace keyspace;
  std::vector<std::thread> workers;
  for (int t = 0; t < 4; ++t) {
    workers.emplace_back([&keyspace, t] {
      for (int i = 0; i < 1000; ++i) {
        const std::string key = "k" + std::to_string((t * 1000 + i) % 300);
        (void)keyspace.set(key, Value::makeString("v" + std::to_string(i)));
        (void)keyspace.get(key);
        if (i % 7 == 0) {
          (void)keyspace.removeOne(key);
        }
        if (i % 11 == 0) {
          (void)keyspace.listPush("list" + std::to_string(t), {"item"}, false);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  SUCCEED() << "no crash, no deadlock, no assertion failure under mixed concurrent load";
}

TEST(Keyspace, MatchingKeysFindsWhatWasStored) {
  Keyspace keyspace;
  ASSERT_TRUE(keyspace.set("user:1", Value::makeString("a")).ok());
  ASSERT_TRUE(keyspace.set("user:2", Value::makeString("b")).ok());
  ASSERT_TRUE(keyspace.set("order:1", Value::makeString("c")).ok());

  std::vector<std::string> users = keyspace.matchingKeys("user:*");
  std::sort(users.begin(), users.end());
  EXPECT_EQ(users, (std::vector<std::string>{"user:1", "user:2"}));
  EXPECT_EQ(keyspace.matchingKeys("*").size(), 3u);
}
