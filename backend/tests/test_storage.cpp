#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "bourse/cache/keyspace.hpp"
#include "bourse/core/file.hpp"
#include "bourse/storage/snapshot.hpp"
#include "bourse/storage/wal.hpp"

using namespace bourse;
using namespace bourse::storage;

namespace {

/// Gives each test its own directory and removes it afterwards, so a failing
/// test cannot poison the next one.
class TempDir {
 public:
  TempDir() {
    path_ = std::filesystem::temp_directory_path() /
            ("bourse-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] std::string file(std::string_view name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

}  // namespace

// ---------------------------------------------------------------------------
// CRC
// ---------------------------------------------------------------------------

TEST(Crc32, MatchesKnownVectors) {
  // The standard IEEE check value: CRC-32 of "123456789" is 0xCBF43926.
  const std::string input = "123456789";
  EXPECT_EQ(crc32(input.data(), input.size()), 0xCBF43926u);
  EXPECT_EQ(crc32("", 0), 0u);
}

TEST(Crc32, DetectsSingleBitFlips) {
  std::string data = "the quick brown fox";
  const std::uint32_t original = crc32(data.data(), data.size());
  data[5] = static_cast<char>(data[5] ^ 0x01);
  EXPECT_NE(crc32(data.data(), data.size()), original);
}

// ---------------------------------------------------------------------------
// WAL
// ---------------------------------------------------------------------------

TEST(WriteAheadLog, AppendsAndReplaysInOrder) {
  TempDir dir;
  WriteAheadLog::Options options;
  options.path = dir.file("test.wal");
  options.sync_policy = WriteAheadLog::SyncPolicy::kEveryWrite;

  {
    Result<std::unique_ptr<WriteAheadLog>> log = WriteAheadLog::open(options);
    ASSERT_TRUE(log.ok()) << log.status().toString();
    ASSERT_TRUE(log.value()->append({"SET", "a", "1"}).ok());
    ASSERT_TRUE(log.value()->append({"SET", "b", "2"}).ok());
    ASSERT_TRUE(log.value()->append({"DEL", "a"}).ok());
    EXPECT_EQ(log.value()->recordCount(), 3u);
  }

  Result<std::unique_ptr<WriteAheadLog>> reopened = WriteAheadLog::open(options);
  ASSERT_TRUE(reopened.ok());

  std::vector<std::vector<std::string>> replayed;
  Result<std::size_t> count =
      reopened.value()->replay([&](const WalRecord& record) { replayed.push_back(record.argv); });
  ASSERT_TRUE(count.ok()) << count.status().toString();
  EXPECT_EQ(count.value(), 3u);
  ASSERT_EQ(replayed.size(), 3u);
  EXPECT_EQ(replayed[0], (std::vector<std::string>{"SET", "a", "1"}));
  EXPECT_EQ(replayed[2], (std::vector<std::string>{"DEL", "a"}));
}

TEST(WriteAheadLog, PreservesBinaryAndEmptyArguments) {
  TempDir dir;
  WriteAheadLog::Options options;
  options.path = dir.file("binary.wal");

  const std::string payload("a\0b\r\n", 5);
  {
    Result<std::unique_ptr<WriteAheadLog>> log = WriteAheadLog::open(options);
    ASSERT_TRUE(log.ok());
    ASSERT_TRUE(log.value()->append({"SET", "k", payload, ""}).ok());
  }

  Result<std::unique_ptr<WriteAheadLog>> reopened = WriteAheadLog::open(options);
  ASSERT_TRUE(reopened.ok());
  std::vector<std::string> argv;
  ASSERT_TRUE(reopened.value()->replay([&](const WalRecord& record) { argv = record.argv; }).ok());
  ASSERT_EQ(argv.size(), 4u);
  EXPECT_EQ(argv[2], payload);
  EXPECT_EQ(argv[2].size(), 5u);
  EXPECT_TRUE(argv[3].empty());
}

TEST(WriteAheadLog, TruncatesATornTailAndKeepsGoing) {
  // The crash case the checksum exists for: a record that was only partly
  // written must be discarded, and everything before it must survive.
  TempDir dir;
  WriteAheadLog::Options options;
  options.path = dir.file("torn.wal");

  {
    Result<std::unique_ptr<WriteAheadLog>> log = WriteAheadLog::open(options);
    ASSERT_TRUE(log.ok());
    ASSERT_TRUE(log.value()->append({"SET", "good", "1"}).ok());
    ASSERT_TRUE(log.value()->append({"SET", "alsogood", "2"}).ok());
  }

  // Append a plausible-looking header with no payload behind it.
  {
    Result<File> file = File::open(options.path, File::Mode::kReadWrite);
    ASSERT_TRUE(file.ok());
    Result<std::uint64_t> size = file.value().size();
    ASSERT_TRUE(size.ok());
    // A plausible-looking record header -- magic, an absurd length, a
    // checksum -- with no payload behind it, which is exactly what a crash
    // mid-write leaves.
    //
    // Built by appending rather than as one literal with a hand-counted
    // length: the escapes make the true size non-obvious, and getting it wrong
    // reads past the end of the literal. AddressSanitizer caught precisely
    // that here.
    std::string garbage;
    garbage.append("BOUR");                 // magic
    garbage.append("\xff\xff\x00\x00", 4);  // length prefix
    garbage.append("\x11\x22\x33\x44", 4);  // checksum
    garbage.append("partial");              // truncated payload
    ASSERT_TRUE(file.value().writeAt(size.value(), garbage.data(), garbage.size()).ok());
  }

  Result<std::unique_ptr<WriteAheadLog>> reopened = WriteAheadLog::open(options);
  ASSERT_TRUE(reopened.ok());
  std::size_t seen = 0;
  std::uint64_t truncated = 0;
  Result<std::size_t> count = reopened.value()->replay([&](const WalRecord&) { ++seen; }, &truncated);
  ASSERT_TRUE(count.ok()) << count.status().toString();
  EXPECT_EQ(seen, 2u) << "committed records were lost";
  EXPECT_GT(truncated, 0u) << "the torn tail was not detected";

  // The log must be usable again, and the repaired file must round-trip.
  ASSERT_TRUE(reopened.value()->append({"SET", "after", "3"}).ok());
  ASSERT_TRUE(reopened.value()->sync().ok());

  Result<std::unique_ptr<WriteAheadLog>> third = WriteAheadLog::open(options);
  ASSERT_TRUE(third.ok());
  std::size_t final_count = 0;
  ASSERT_TRUE(third.value()->replay([&](const WalRecord&) { ++final_count; }).ok());
  EXPECT_EQ(final_count, 3u);
}

TEST(WriteAheadLog, RejectsAFileThatIsNotAWal) {
  TempDir dir;
  const std::string path = dir.file("bogus.wal");
  ASSERT_TRUE(File::writeWholeFile(path, "this is definitely not a write-ahead log").ok());

  WriteAheadLog::Options options;
  options.path = path;
  Result<std::unique_ptr<WriteAheadLog>> log = WriteAheadLog::open(options);
  ASSERT_TRUE(log.ok());

  Result<std::size_t> replayed = log.value()->replay([](const WalRecord&) {});
  ASSERT_FALSE(replayed.ok());
  EXPECT_EQ(replayed.status().code(), ErrorCode::kCorruption);
}

TEST(WriteAheadLog, ResetEmptiesTheLog) {
  TempDir dir;
  WriteAheadLog::Options options;
  options.path = dir.file("reset.wal");

  Result<std::unique_ptr<WriteAheadLog>> log = WriteAheadLog::open(options);
  ASSERT_TRUE(log.ok());
  ASSERT_TRUE(log.value()->append({"SET", "a", "1"}).ok());
  EXPECT_GT(log.value()->bytesWritten(), 0u);

  ASSERT_TRUE(log.value()->reset().ok());
  EXPECT_EQ(log.value()->bytesWritten(), 0u);
  EXPECT_EQ(log.value()->recordCount(), 0u);
}

TEST(WriteAheadLog, ParsesSyncPolicies) {
  WriteAheadLog::SyncPolicy policy{};
  EXPECT_TRUE(WriteAheadLog::parseSyncPolicy("always", policy));
  EXPECT_EQ(policy, WriteAheadLog::SyncPolicy::kEveryWrite);
  EXPECT_TRUE(WriteAheadLog::parseSyncPolicy("everysec", policy));
  EXPECT_EQ(policy, WriteAheadLog::SyncPolicy::kEverySecond);
  EXPECT_TRUE(WriteAheadLog::parseSyncPolicy("never", policy));
  EXPECT_FALSE(WriteAheadLog::parseSyncPolicy("sometimes", policy));
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

TEST(SnapshotTest, RoundTripsEveryValueType) {
  TempDir dir;
  const std::string path = dir.file("keyspace.snapshot");

  cache::Keyspace source;
  ASSERT_TRUE(source.set("text", cache::Value::makeString("hello world")).ok());
  ASSERT_TRUE(source.incrementBy("counter", 42).ok());
  ASSERT_TRUE(source.listPush("list", {"a", "b", "c"}, false).ok());
  ASSERT_TRUE(source.hashSet("hash", {{"x", "1"}, {"y", "2"}}).ok());
  ASSERT_TRUE(source.setAdd("set", {"p", "q"}).ok());
  ASSERT_TRUE(source.set("ttl", cache::Value::makeString("v"), 3'600'000).ok());

  Result<SnapshotStats> saved = Snapshot::save(source, path);
  ASSERT_TRUE(saved.ok()) << saved.status().toString();
  EXPECT_EQ(saved.value().keys, 6u);
  EXPECT_TRUE(Snapshot::exists(path));

  cache::Keyspace restored;
  Result<SnapshotStats> loaded = Snapshot::load(restored, path);
  ASSERT_TRUE(loaded.ok()) << loaded.status().toString();
  EXPECT_EQ(loaded.value().keys, 6u);

  EXPECT_EQ(*restored.get("text").value(), "hello world");
  EXPECT_EQ(*restored.get("counter").value(), "42");
  EXPECT_EQ(restored.listLength("list").value(), 3u);
  EXPECT_EQ(restored.listRange("list", 0, -1).value(), (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_EQ(*restored.hashGet("hash", "x").value(), "1");
  EXPECT_EQ(restored.setCardinality("set").value(), 2u);
  EXPECT_GT(restored.ttlMillis("ttl"), 3'500'000);
}

TEST(SnapshotTest, DropsKeysThatExpiredWhileDown) {
  TempDir dir;
  const std::string path = dir.file("expired.snapshot");

  cache::Keyspace source;
  ASSERT_TRUE(source.set("alive", cache::Value::makeString("v"), 3'600'000).ok());
  ASSERT_TRUE(source.set("doomed", cache::Value::makeString("v"), 30).ok());
  ASSERT_TRUE(Snapshot::save(source, path).ok());

  std::this_thread::sleep_for(std::chrono::milliseconds(60));

  cache::Keyspace restored;
  Result<SnapshotStats> loaded = Snapshot::load(restored, path);
  ASSERT_TRUE(loaded.ok());
  EXPECT_TRUE(restored.exists("alive"));
  EXPECT_FALSE(restored.exists("doomed")) << "an expired key came back to life across a restart";
}

TEST(SnapshotTest, RefusesToLoadADamagedImage) {
  TempDir dir;
  const std::string path = dir.file("damaged.snapshot");

  cache::Keyspace source;
  ASSERT_TRUE(source.set("k", cache::Value::makeString("value")).ok());
  ASSERT_TRUE(Snapshot::save(source, path).ok());

  // Flip a byte in the middle of the body.
  Result<std::string> contents = File::readWholeFile(path);
  ASSERT_TRUE(contents.ok());
  std::string image = contents.value();
  ASSERT_GT(image.size(), 20u);
  image[image.size() / 2] = static_cast<char>(image[image.size() / 2] ^ 0xFF);
  ASSERT_TRUE(File::writeWholeFile(path, image).ok());

  cache::Keyspace restored;
  Result<SnapshotStats> loaded = Snapshot::load(restored, path);
  ASSERT_FALSE(loaded.ok()) << "a corrupted snapshot was accepted";
  EXPECT_EQ(loaded.status().code(), ErrorCode::kCorruption);
}

TEST(SnapshotTest, RejectsForeignFiles) {
  TempDir dir;
  const std::string path = dir.file("foreign.snapshot");
  ASSERT_TRUE(File::writeWholeFile(path, "PNG\x89 definitely not a snapshot").ok());

  cache::Keyspace restored;
  EXPECT_FALSE(Snapshot::load(restored, path).ok());
}

TEST(SnapshotTest, SaveIsAtomic) {
  TempDir dir;
  const std::string path = dir.file("atomic.snapshot");

  cache::Keyspace source;
  ASSERT_TRUE(source.set("k", cache::Value::makeString("v")).ok());
  ASSERT_TRUE(Snapshot::save(source, path).ok());
  // The temp file must not survive a successful save.
  EXPECT_FALSE(File::exists(path + ".tmp"));
  EXPECT_TRUE(File::exists(path));
}

// ---------------------------------------------------------------------------
// File
// ---------------------------------------------------------------------------

TEST(FileTest, PositionalIoRoundTrips) {
  TempDir dir;
  Result<File> file = File::open(dir.file("io.bin"), File::Mode::kTruncateReadWrite);
  ASSERT_TRUE(file.ok());

  const std::string payload = "hello positional world";
  ASSERT_TRUE(file.value().writeAt(0, payload.data(), payload.size()).ok());

  std::string read(payload.size(), '\0');
  ASSERT_TRUE(file.value().readAt(0, read.data(), read.size()).ok());
  EXPECT_EQ(read, payload);

  Result<std::uint64_t> size = file.value().size();
  ASSERT_TRUE(size.ok());
  EXPECT_EQ(size.value(), payload.size());

  ASSERT_TRUE(file.value().truncate(5).ok());
  EXPECT_EQ(file.value().size().value(), 5u);
}

TEST(FileTest, ShortReadIsAnErrorNotSilentTruncation) {
  TempDir dir;
  Result<File> file = File::open(dir.file("short.bin"), File::Mode::kTruncateReadWrite);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(file.value().writeAt(0, "abc", 3).ok());

  std::string buffer(100, '\0');
  EXPECT_FALSE(file.value().readAt(0, buffer.data(), buffer.size()).ok())
      << "reading past EOF must fail loudly";
}

TEST(FileTest, MoveTransfersOwnership) {
  TempDir dir;
  Result<File> file = File::open(dir.file("move.bin"), File::Mode::kCreateReadWrite);
  ASSERT_TRUE(file.ok());

  File original = std::move(file).value();
  const int fd = original.fd();
  ASSERT_GE(fd, 0);

  File moved = std::move(original);
  EXPECT_EQ(moved.fd(), fd);
  EXPECT_FALSE(original.isOpen()) << "the moved-from file must not still own the descriptor";
}
