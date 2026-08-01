#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "bourse/core/byte_buffer.hpp"
#include "bourse/core/clock.hpp"
#include "bourse/core/metrics.hpp"
#include "bourse/core/object_pool.hpp"
#include "bourse/core/result.hpp"
#include "bourse/core/spsc_ring.hpp"
#include "bourse/core/thread_pool.hpp"

using namespace bourse;

// ---------------------------------------------------------------------------
// Status / Result
// ---------------------------------------------------------------------------

TEST(Status, DefaultIsOk) {
  Status status;
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(status.code(), ErrorCode::kOk);
  EXPECT_EQ(status.toString(), "OK");
}

TEST(Status, CarriesCodeAndMessage) {
  const Status status = Status::notFound("no such key");
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), ErrorCode::kNotFound);
  EXPECT_EQ(status.message(), "no such key");
  EXPECT_EQ(status.toString(), "NOT_FOUND: no such key");
}

TEST(ResultTest, HoldsValueOrError) {
  Result<int> good(42);
  ASSERT_TRUE(good.ok());
  EXPECT_EQ(good.value(), 42);

  Result<int> bad(Status::ioError("disk on fire"));
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.status().code(), ErrorCode::kIoError);
  EXPECT_EQ(bad.valueOr(-1), -1);
}

TEST(ResultTest, MovesNonCopyablePayloads) {
  Result<std::unique_ptr<int>> result(std::make_unique<int>(7));
  ASSERT_TRUE(result.ok());
  std::unique_ptr<int> taken = std::move(result).value();
  ASSERT_NE(taken, nullptr);
  EXPECT_EQ(*taken, 7);
}

// ---------------------------------------------------------------------------
// ByteBuffer
// ---------------------------------------------------------------------------

TEST(ByteBuffer, AppendAndRetrieve) {
  ByteBuffer buffer;
  EXPECT_TRUE(buffer.empty());

  buffer.append("hello world");
  EXPECT_EQ(buffer.readable(), 11u);
  EXPECT_EQ(buffer.view(), "hello world");

  EXPECT_EQ(buffer.retrieveAsString(5), "hello");
  EXPECT_EQ(buffer.view(), " world");

  buffer.retrieveAll();
  EXPECT_TRUE(buffer.empty());
}

TEST(ByteBuffer, FindLocatesDelimiters) {
  ByteBuffer buffer;
  buffer.append("*2\r\n$3\r\nGET\r\n");
  EXPECT_EQ(buffer.findCRLF(), 2u);
  EXPECT_EQ(buffer.find("GET"), 8u);
  EXPECT_EQ(buffer.find("MISSING"), ByteBuffer::npos);
}

TEST(ByteBuffer, CompactsInsteadOfGrowingWhenPossible) {
  // Consume most of the buffer, then append again. The implementation should
  // reclaim the read region by compacting rather than reallocating -- this is
  // what keeps a long-lived connection allocation-free in steady state.
  ByteBuffer buffer(64);
  buffer.append(std::string(60, 'a'));
  buffer.retrieve(58);
  const std::size_t capacity_before = buffer.capacity();

  buffer.append(std::string(40, 'b'));
  EXPECT_EQ(buffer.capacity(), capacity_before) << "buffer reallocated when compaction would have sufficed";
  EXPECT_EQ(buffer.readable(), 42u);
}

TEST(ByteBuffer, GrowsWhenCompactionIsInsufficient) {
  ByteBuffer buffer(16);
  buffer.append(std::string(4096, 'x'));
  EXPECT_GE(buffer.capacity(), 4096u);
  EXPECT_EQ(buffer.readable(), 4096u);
}

TEST(ByteBuffer, PrependWritesInFrontWithoutMoving) {
  ByteBuffer buffer;
  buffer.append("payload");
  const char header[2] = {'\x01', '\x02'};
  ASSERT_TRUE(buffer.prepend(header, sizeof(header)));
  EXPECT_EQ(buffer.readable(), 9u);
  EXPECT_EQ(buffer.view().substr(2), "payload");
}

TEST(ByteBuffer, ShrinkReleasesCapacity) {
  ByteBuffer buffer;
  buffer.append(std::string(100000, 'x'));
  buffer.retrieve(99990);
  buffer.shrink(128);
  EXPECT_LT(buffer.capacity(), 4096u);
  EXPECT_EQ(buffer.readable(), 10u);
}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

TEST(Histogram, EmptyReportsZeros) {
  Histogram histogram;
  EXPECT_EQ(histogram.count(), 0u);
  EXPECT_EQ(histogram.percentile(99.0), 0u);
}

TEST(Histogram, SmallValuesAreExact) {
  Histogram histogram;
  for (std::uint64_t v = 0; v < 16; ++v) {
    histogram.record(v);
  }
  EXPECT_EQ(histogram.count(), 16u);
  EXPECT_EQ(histogram.min(), 0u);
  EXPECT_EQ(histogram.max(), 15u);
}

TEST(Histogram, PercentilesLandWithinBucketResolution) {
  Histogram histogram;
  for (int i = 1; i <= 1000; ++i) {
    histogram.record(static_cast<std::uint64_t>(i));
  }
  // 16 sub-buckets per octave bounds relative error at 1/16 = 6.25%.
  const std::uint64_t p50 = histogram.percentile(50.0);
  const std::uint64_t p99 = histogram.percentile(99.0);
  EXPECT_NEAR(static_cast<double>(p50), 500.0, 500.0 * 0.0625 + 1.0);
  EXPECT_NEAR(static_cast<double>(p99), 990.0, 990.0 * 0.0625 + 1.0);
  EXPECT_LE(p50, p99);
}

TEST(Histogram, ConcurrentRecordingIsLossless) {
  Histogram histogram;
  constexpr int kThreads = 4;
  constexpr int kPerThread = 5000;

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&histogram] {
      for (int i = 0; i < kPerThread; ++i) {
        histogram.record(static_cast<std::uint64_t>(i % 997) + 1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  EXPECT_EQ(histogram.count(), static_cast<std::uint64_t>(kThreads) * kPerThread);
}

TEST(MetricsRegistry, CountersAndGaugesRoundTrip) {
  MetricsRegistry registry;
  registry.counter("requests").increment(5);
  registry.counter("requests").increment();
  registry.gauge("connections").set(3);
  registry.gauge("connections").add(2);

  EXPECT_EQ(registry.counter("requests").value(), 6u);
  EXPECT_EQ(registry.gauge("connections").value(), 5);

  const std::string prometheus = registry.renderPrometheus();
  EXPECT_NE(prometheus.find("requests 6"), std::string::npos);

  const std::string json = registry.renderJson();
  EXPECT_NE(json.find("\"requests\":6"), std::string::npos);
}

// ---------------------------------------------------------------------------
// SpscRing
// ---------------------------------------------------------------------------

TEST(SpscRing, PushPopPreservesOrder) {
  SpscRing<int, 8> ring;
  for (int i = 0; i < 7; ++i) {
    EXPECT_TRUE(ring.push(i));
  }
  EXPECT_FALSE(ring.push(99)) << "ring should report full at capacity-1";

  for (int i = 0; i < 7; ++i) {
    int out = -1;
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out, i);
  }
  int drained = 0;
  EXPECT_FALSE(ring.pop(drained));
}

TEST(SpscRing, SingleProducerSingleConsumerDeliversEverything) {
  // The whole point of the structure: no locks, no lost or duplicated items.
  SpscRing<std::uint64_t, 1024> ring;
  constexpr std::uint64_t kCount = 200000;

  std::thread producer([&ring] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!ring.push(i)) {
        std::this_thread::yield();
      }
    }
  });

  std::uint64_t expected = 0;
  while (expected < kCount) {
    std::uint64_t value = 0;
    if (ring.pop(value)) {
      ASSERT_EQ(value, expected) << "ordering violated at " << expected;
      ++expected;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  EXPECT_EQ(expected, kCount);
}

// ---------------------------------------------------------------------------
// ObjectPool / Arena
// ---------------------------------------------------------------------------

namespace {
struct Tracked {
  static inline int live = 0;
  int value;
  explicit Tracked(int v) : value(v) { ++live; }
  ~Tracked() { --live; }
};
}  // namespace

TEST(ObjectPool, RecyclesMemoryAndRunsDestructors) {
  Tracked::live = 0;
  {
    ObjectPool<Tracked> pool(4);
    Tracked* a = pool.acquire(1);
    Tracked* b = pool.acquire(2);
    EXPECT_EQ(Tracked::live, 2);
    EXPECT_EQ(a->value, 1);
    EXPECT_EQ(b->value, 2);
    EXPECT_EQ(pool.live(), 2u);

    pool.release(a);
    EXPECT_EQ(Tracked::live, 1);

    // The freed slot must be handed back out rather than growing the pool.
    Tracked* c = pool.acquire(3);
    EXPECT_EQ(c, a);
    pool.release(b);
    pool.release(c);
  }
  EXPECT_EQ(Tracked::live, 0);
}

TEST(ObjectPool, SteadyStateStopsAllocating) {
  ObjectPool<Tracked> pool(64);
  // Warm up, then churn. After warm-up the chunk count must not move -- that is
  // the property that keeps the matching-engine hot path free of operator new.
  std::vector<Tracked*> held;
  for (int i = 0; i < 64; ++i) {
    held.push_back(pool.acquire(i));
  }
  const std::size_t chunks_after_warmup = pool.chunkCount();
  for (Tracked* item : held) {
    pool.release(item);
  }
  for (int round = 0; round < 1000; ++round) {
    Tracked* item = pool.acquire(round);
    pool.release(item);
  }
  EXPECT_EQ(pool.chunkCount(), chunks_after_warmup);
}

TEST(Arena, BumpAllocatesAndResets) {
  Arena arena(1024);
  int* a = arena.create<int>(1);
  double* b = arena.create<double>(2.5);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(*a, 1);
  EXPECT_DOUBLE_EQ(*b, 2.5);
  EXPECT_GT(arena.bytesUsed(), 0u);

  arena.reset();
  EXPECT_EQ(arena.bytesUsed(), 0u);
}

TEST(Arena, RespectsAlignment) {
  Arena arena;
  for (int i = 0; i < 100; ++i) {
    void* p = arena.allocate(3, 16);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 16, 0u);
  }
}

// ---------------------------------------------------------------------------
// ThreadPool
// ---------------------------------------------------------------------------

TEST(ThreadPool, RunsSubmittedWorkAndReturnsFutures) {
  ThreadPool pool(4);
  std::vector<std::future<int>> futures;
  futures.reserve(100);
  for (int i = 0; i < 100; ++i) {
    futures.push_back(pool.submit([](int x) { return x * 2; }, i));
  }
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(futures[static_cast<std::size_t>(i)].get(), i * 2);
  }
}

TEST(ThreadPool, TryScheduleShedsLoadAtTheBound) {
  // A bounded queue must refuse rather than grow. Block the single worker, then
  // fill the queue and confirm the next submission is rejected.
  ThreadPool pool(1, 2);
  std::atomic<bool> release{false};
  ASSERT_TRUE(pool.tryScheduleTask([&release] {
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }));

  int accepted = 0;
  for (int i = 0; i < 10; ++i) {
    if (pool.tryScheduleTask([] {})) {
      ++accepted;
    }
  }
  EXPECT_LE(accepted, 2) << "bounded queue accepted more than its depth";
  release.store(true, std::memory_order_release);
  pool.drain();
}

TEST(ThreadPool, ExceptionInTaskDoesNotKillTheWorker) {
  ThreadPool pool(2);
  ASSERT_TRUE(pool.tryScheduleTask([] { throw std::runtime_error("boom"); }));
  // The pool must still be usable afterwards.
  auto future = pool.submit([] { return 5; });
  EXPECT_EQ(future.get(), 5);
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

TEST(Clock, StopwatchMeasuresForwardProgress) {
  Stopwatch watch;
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_GE(watch.elapsedMillis(), 4.0);
}

TEST(Clock, TimestampFormatIsStable) {
  const std::string formatted = formatTimestamp(0);
  EXPECT_EQ(formatted.size(), 23u);  // YYYY-MM-DD HH:MM:SS.mmm
  EXPECT_EQ(formatted[4], '-');
  EXPECT_EQ(formatted[10], ' ');
  EXPECT_EQ(formatted[19], '.');
}
