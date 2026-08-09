#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "bourse/core/file.hpp"
#include "bourse/core/result.hpp"

namespace bourse::storage {

/// One journalled mutation: the command exactly as the client sent it.
struct WalRecord {
  std::uint64_t sequence = 0;
  std::int64_t timestamp_ms = 0;
  std::vector<std::string> argv;
};

/// CRC-32 (IEEE 802.3 polynomial, reflected). Table built once on first use.
[[nodiscard]] std::uint32_t crc32(const void* data, std::size_t length, std::uint32_t seed = 0);

/// Append-only write-ahead log.
///
/// **On-disk record layout** (all integers little-endian):
///
///     magic u32 | payload_length u32 | crc32 u32 | payload[payload_length]
///     payload := sequence u64 | timestamp_ms i64 | argc u32 | (len u32, bytes)*
///
/// The magic and the CRC exist for exactly one reason: a process killed
/// mid-`write` leaves a torn final record. Replay must be able to tell
/// "corrupt tail from a crash" (stop cleanly, truncate, carry on) from
/// "corrupt middle" (real data loss, refuse to start silently). Without a
/// checksum the replay would happily feed half a command into the keyspace.
///
/// **Durability is a policy, not a constant.** `fsync` per record bounds commit
/// rate at disk latency -- on a spinning disk that is a few hundred writes per
/// second. Syncing on an interval trades a bounded window of writes for orders
/// of magnitude more throughput. Both are offered because neither is the right
/// default for every deployment, and picking one silently would be worse than
/// making the caller choose.
class WriteAheadLog {
 public:
  enum class SyncPolicy : std::uint8_t {
    kNever,        ///< rely on the OS page cache; fastest, loses on host crash
    kEverySecond,  ///< bounded loss window, the practical default
    kEveryWrite,   ///< no acknowledged write is ever lost; slowest
  };

  struct Options {
    std::string path;
    SyncPolicy sync_policy = SyncPolicy::kEverySecond;
  };

  static Result<std::unique_ptr<WriteAheadLog>> open(Options options);
  ~WriteAheadLog();

  WriteAheadLog(const WriteAheadLog&) = delete;
  WriteAheadLog& operator=(const WriteAheadLog&) = delete;

  /// Appends one command. Thread-safe.
  Status append(const std::vector<std::string>& argv);

  /// Forces buffered records to stable storage.
  Status sync();

  /// Called on the server's cron tick; issues an fsync when the policy is
  /// kEverySecond and one is due.
  Status maybeSync();

  /// Replays the log from the start, invoking `apply` per record.
  ///
  /// Returns the number of records replayed. A torn tail is truncated away and
  /// reported through `truncated_bytes`; corruption before the tail is an error.
  Result<std::size_t> replay(const std::function<void(const WalRecord&)>& apply,
                             std::uint64_t* truncated_bytes = nullptr);

  /// Discards the log and starts again. Used after a snapshot has captured
  /// everything the log described.
  Status reset();

  [[nodiscard]] std::uint64_t recordCount() const;
  [[nodiscard]] std::uint64_t bytesWritten() const;

  [[nodiscard]] const std::string& path() const noexcept { return options_.path; }

  [[nodiscard]] SyncPolicy syncPolicy() const noexcept { return options_.sync_policy; }

  [[nodiscard]] static const char* toString(SyncPolicy policy) noexcept;
  [[nodiscard]] static bool parseSyncPolicy(std::string_view text, SyncPolicy& out) noexcept;

  static constexpr std::uint32_t kRecordMagic = 0x424F5552;  // "BOUR"
  static constexpr std::uint32_t kMaxRecordBytes = 64u * 1024 * 1024;

 private:
  WriteAheadLog(Options options, File file) : options_(std::move(options)), file_(std::move(file)) {}

  static std::string encodeRecord(const WalRecord& record);

  Options options_;
  mutable std::mutex mutex_;
  File file_;
  std::uint64_t next_sequence_ = 1;
  std::uint64_t bytes_written_ = 0;
  std::uint64_t records_ = 0;
  std::int64_t last_sync_ms_ = 0;
  bool dirty_ = false;
};

}  // namespace bourse::storage
