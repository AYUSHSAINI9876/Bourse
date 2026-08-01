#include "bourse/storage/wal.hpp"

#include <array>
#include <cctype>
#include <cstring>

#include "bourse/core/clock.hpp"
#include "bourse/core/logger.hpp"

namespace bourse::storage {
namespace {

/// Little-endian appenders. Explicit rather than memcpy of the native
/// representation so a log written on one machine reads on another.
void putU32(std::string& out, std::uint32_t value) {
  out.push_back(static_cast<char>(value & 0xFF));
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>((value >> 16) & 0xFF));
  out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

void putU64(std::string& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

std::uint32_t readU32(const unsigned char* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t readU64(const unsigned char* p) {
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | p[i];
  }
  return value;
}

constexpr std::size_t kHeaderBytes = 12;  // magic + length + crc

}  // namespace

std::uint32_t crc32(const void* data, std::size_t length, std::uint32_t seed) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> generated{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1U) != 0 ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
      }
      generated[i] = value;
    }
    return generated;
  }();

  std::uint32_t crc = ~seed;
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < length; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
  }
  return ~crc;
}

const char* WriteAheadLog::toString(SyncPolicy policy) noexcept {
  switch (policy) {
    case SyncPolicy::kNever: return "never";
    case SyncPolicy::kEverySecond: return "everysec";
    case SyncPolicy::kEveryWrite: return "always";
  }
  return "everysec";
}

bool WriteAheadLog::parseSyncPolicy(std::string_view text, SyncPolicy& out) noexcept {
  std::string lowered;
  lowered.reserve(text.size());
  for (char c : text) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lowered == "never" || lowered == "no") {
    out = SyncPolicy::kNever;
    return true;
  }
  if (lowered == "everysec" || lowered == "everysecond") {
    out = SyncPolicy::kEverySecond;
    return true;
  }
  if (lowered == "always" || lowered == "everywrite") {
    out = SyncPolicy::kEveryWrite;
    return true;
  }
  return false;
}

Result<std::unique_ptr<WriteAheadLog>> WriteAheadLog::open(Options options) {
  Result<File> file = File::open(options.path, File::Mode::kCreateReadWrite);
  if (!file.ok()) {
    return file.status();
  }

  Result<std::uint64_t> size = file.value().size();
  if (!size.ok()) {
    return size.status();
  }

  auto log = std::unique_ptr<WriteAheadLog>(new WriteAheadLog(std::move(options), std::move(file).value()));
  log->bytes_written_ = size.value();
  log->last_sync_ms_ = nowMillis();
  return log;
}

WriteAheadLog::~WriteAheadLog() {
  // Best effort: a destructor cannot report an error, but losing acknowledged
  // writes on a clean shutdown would be inexcusable.
  (void)sync();
}

std::string WriteAheadLog::encodeRecord(const WalRecord& record) {
  std::string payload;
  payload.reserve(64);
  putU64(payload, record.sequence);
  putU64(payload, static_cast<std::uint64_t>(record.timestamp_ms));
  putU32(payload, static_cast<std::uint32_t>(record.argv.size()));
  for (const std::string& argument : record.argv) {
    putU32(payload, static_cast<std::uint32_t>(argument.size()));
    payload.append(argument);
  }

  std::string framed;
  framed.reserve(payload.size() + kHeaderBytes);
  putU32(framed, kRecordMagic);
  putU32(framed, static_cast<std::uint32_t>(payload.size()));
  putU32(framed, crc32(payload.data(), payload.size()));
  framed.append(payload);
  return framed;
}

Status WriteAheadLog::append(const std::vector<std::string>& argv) {
  if (argv.empty()) {
    return Status::success();
  }

  std::lock_guard<std::mutex> lock(mutex_);

  WalRecord record;
  record.sequence = next_sequence_++;
  record.timestamp_ms = nowMillis();
  record.argv = argv;

  const std::string framed = encodeRecord(record);
  if (framed.size() > kMaxRecordBytes) {
    return Status::invalidArgument("WAL record exceeds the maximum record size");
  }

  BOURSE_TRY(file_.writeAt(bytes_written_, framed.data(), framed.size()));
  bytes_written_ += framed.size();
  ++records_;
  dirty_ = true;

  if (options_.sync_policy == SyncPolicy::kEveryWrite) {
    BOURSE_TRY(file_.sync());
    dirty_ = false;
    last_sync_ms_ = nowMillis();
  }
  return Status::success();
}

Status WriteAheadLog::sync() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!dirty_ || !file_.isOpen()) {
    return Status::success();
  }
  BOURSE_TRY(file_.sync());
  dirty_ = false;
  last_sync_ms_ = nowMillis();
  return Status::success();
}

Status WriteAheadLog::maybeSync() {
  if (options_.sync_policy != SyncPolicy::kEverySecond) {
    return Status::success();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dirty_ || nowMillis() - last_sync_ms_ < 1000) {
      return Status::success();
    }
  }
  return sync();
}

Result<std::size_t> WriteAheadLog::replay(const std::function<void(const WalRecord&)>& apply,
                                          std::uint64_t* truncated_bytes) {
  std::lock_guard<std::mutex> lock(mutex_);

  Result<std::uint64_t> size = file_.size();
  if (!size.ok()) {
    return size.status();
  }
  const std::uint64_t total = size.value();

  std::size_t replayed = 0;
  std::uint64_t offset = 0;
  std::uint64_t good_offset = 0;
  std::vector<unsigned char> buffer;

  while (offset + kHeaderBytes <= total) {
    std::array<unsigned char, kHeaderBytes> header{};
    Status read = file_.readAt(offset, header.data(), header.size());
    if (!read.ok()) {
      break;  // torn header at the tail
    }

    const std::uint32_t magic = readU32(header.data());
    const std::uint32_t length = readU32(header.data() + 4);
    const std::uint32_t expected_crc = readU32(header.data() + 8);

    if (magic != kRecordMagic || length > kMaxRecordBytes) {
      // Garbage where a record header should be. If we have already read
      // records this is a torn tail; if it is the very first byte the file is
      // not one of ours.
      if (offset == 0) {
        return Status::corruption("WAL header magic mismatch at offset 0; not a Bourse WAL");
      }
      break;
    }
    if (offset + kHeaderBytes + length > total) {
      break;  // record was only partially written before the crash
    }

    buffer.resize(length);
    read = file_.readAt(offset + kHeaderBytes, buffer.data(), length);
    if (!read.ok()) {
      break;
    }
    if (crc32(buffer.data(), buffer.size()) != expected_crc) {
      // A checksum failure mid-file is real corruption, not a torn tail --
      // the record after it exists, so this was not the last write.
      const bool has_following_record = offset + kHeaderBytes + length + kHeaderBytes <= total;
      if (has_following_record) {
        return Status::corruption("WAL checksum mismatch at offset " + std::to_string(offset));
      }
      break;
    }

    // ---- decode the payload ---------------------------------------------
    if (length < 20) {
      break;
    }
    WalRecord record;
    record.sequence = readU64(buffer.data());
    record.timestamp_ms = static_cast<std::int64_t>(readU64(buffer.data() + 8));
    const std::uint32_t argc = readU32(buffer.data() + 16);

    std::size_t cursor = 20;
    bool decoded = true;
    record.argv.reserve(argc);
    for (std::uint32_t i = 0; i < argc; ++i) {
      if (cursor + 4 > buffer.size()) {
        decoded = false;
        break;
      }
      const std::uint32_t argument_length = readU32(buffer.data() + cursor);
      cursor += 4;
      if (cursor + argument_length > buffer.size()) {
        decoded = false;
        break;
      }
      record.argv.emplace_back(reinterpret_cast<const char*>(buffer.data() + cursor), argument_length);
      cursor += argument_length;
    }
    if (!decoded) {
      break;
    }

    apply(record);
    ++replayed;
    next_sequence_ = record.sequence + 1;

    offset += kHeaderBytes + length;
    good_offset = offset;
  }

  const std::uint64_t discarded = total - good_offset;
  if (truncated_bytes != nullptr) {
    *truncated_bytes = discarded;
  }
  if (discarded > 0) {
    // Truncating is not optional: leaving the torn tail in place would make
    // the next append land after garbage, and the following replay would stop
    // there and silently lose everything written afterwards.
    BOURSE_LOG_WARN("WAL: discarding ", discarded, " byte(s) of torn tail after a crash");
    BOURSE_TRY(file_.truncate(good_offset));
  }

  bytes_written_ = good_offset;
  records_ = replayed;
  return replayed;
}

Status WriteAheadLog::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  BOURSE_TRY(file_.truncate(0));
  BOURSE_TRY(file_.sync());
  bytes_written_ = 0;
  records_ = 0;
  next_sequence_ = 1;
  dirty_ = false;
  return Status::success();
}

std::uint64_t WriteAheadLog::recordCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return records_;
}

std::uint64_t WriteAheadLog::bytesWritten() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bytes_written_;
}

}  // namespace bourse::storage
