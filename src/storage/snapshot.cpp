#include "bourse/storage/snapshot.hpp"

#include <cstring>
#include <vector>

#include "bourse/core/clock.hpp"
#include "bourse/core/file.hpp"
#include "bourse/core/logger.hpp"
#include "bourse/storage/wal.hpp"

namespace bourse::storage {
namespace {

using cache::Entry;
using cache::HashValue;
using cache::ListValue;
using cache::SetValue;
using cache::Value;
using cache::ValueType;

void putU32(std::string& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

void putU64(std::string& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

void putString(std::string& out, std::string_view text) {
  putU32(out, static_cast<std::uint32_t>(text.size()));
  out.append(text);
}

class Reader {
 public:
  explicit Reader(std::string_view data) : data_(data) {}

  [[nodiscard]] bool readU32(std::uint32_t& out) {
    if (cursor_ + 4 > data_.size()) {
      return false;
    }
    out = 0;
    for (int i = 3; i >= 0; --i) {
      out = (out << 8) | static_cast<unsigned char>(data_[cursor_ + static_cast<std::size_t>(i)]);
    }
    cursor_ += 4;
    return true;
  }

  [[nodiscard]] bool readU64(std::uint64_t& out) {
    if (cursor_ + 8 > data_.size()) {
      return false;
    }
    out = 0;
    for (int i = 7; i >= 0; --i) {
      out = (out << 8) | static_cast<unsigned char>(data_[cursor_ + static_cast<std::size_t>(i)]);
    }
    cursor_ += 8;
    return true;
  }

  [[nodiscard]] bool readByte(std::uint8_t& out) {
    if (cursor_ + 1 > data_.size()) {
      return false;
    }
    out = static_cast<std::uint8_t>(data_[cursor_++]);
    return true;
  }

  [[nodiscard]] bool readString(std::string& out) {
    std::uint32_t length = 0;
    if (!readU32(length) || cursor_ + length > data_.size()) {
      return false;
    }
    out.assign(data_.data() + cursor_, length);
    cursor_ += length;
    return true;
  }

  [[nodiscard]] std::size_t offset() const noexcept { return cursor_; }

 private:
  std::string_view data_;
  std::size_t cursor_ = 0;
};

void encodeValue(std::string& out, const Value& value) {
  out.push_back(static_cast<char>(value.type()));
  switch (value.type()) {
    case ValueType::kNone:
      break;
    case ValueType::kString:
      putString(out, value.toStringValue());
      break;
    case ValueType::kInteger:
      putU64(out, static_cast<std::uint64_t>(value.asInteger().valueOr(0)));
      break;
    case ValueType::kList: {
      const ListValue& list = value.list();
      putU32(out, static_cast<std::uint32_t>(list.size()));
      for (const std::string& item : list) {
        putString(out, item);
      }
      break;
    }
    case ValueType::kHash: {
      const HashValue& hash = value.hash();
      putU32(out, static_cast<std::uint32_t>(hash.size()));
      for (const auto& [field, item] : hash) {
        putString(out, field);
        putString(out, item);
      }
      break;
    }
    case ValueType::kSet: {
      const SetValue& set = value.set();
      putU32(out, static_cast<std::uint32_t>(set.size()));
      for (const std::string& member : set) {
        putString(out, member);
      }
      break;
    }
  }
}

bool decodeValue(Reader& reader, Value& out) {
  std::uint8_t tag = 0;
  if (!reader.readByte(tag)) {
    return false;
  }

  switch (static_cast<ValueType>(tag)) {
    case ValueType::kNone:
      out = Value{};
      return true;
    case ValueType::kString: {
      std::string text;
      if (!reader.readString(text)) {
        return false;
      }
      out = Value::makeString(std::move(text));
      return true;
    }
    case ValueType::kInteger: {
      std::uint64_t raw = 0;
      if (!reader.readU64(raw)) {
        return false;
      }
      out = Value::makeInteger(static_cast<std::int64_t>(raw));
      return true;
    }
    case ValueType::kList: {
      std::uint32_t count = 0;
      if (!reader.readU32(count)) {
        return false;
      }
      ListValue list;
      for (std::uint32_t i = 0; i < count; ++i) {
        std::string item;
        if (!reader.readString(item)) {
          return false;
        }
        list.push_back(std::move(item));
      }
      out = Value::makeList(std::move(list));
      return true;
    }
    case ValueType::kHash: {
      std::uint32_t count = 0;
      if (!reader.readU32(count)) {
        return false;
      }
      HashValue hash;
      hash.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        std::string field;
        std::string item;
        if (!reader.readString(field) || !reader.readString(item)) {
          return false;
        }
        hash.emplace(std::move(field), std::move(item));
      }
      out = Value::makeHash(std::move(hash));
      return true;
    }
    case ValueType::kSet: {
      std::uint32_t count = 0;
      if (!reader.readU32(count)) {
        return false;
      }
      SetValue set;
      set.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        std::string member;
        if (!reader.readString(member)) {
          return false;
        }
        set.insert(std::move(member));
      }
      out = Value::makeSet(std::move(set));
      return true;
    }
  }
  return false;
}

}  // namespace

bool Snapshot::exists(const std::string& path) { return File::exists(path); }

Result<SnapshotStats> Snapshot::save(const cache::Keyspace& keyspace, const std::string& path) {
  const Stopwatch watch;

  std::string body;
  body.reserve(1 << 16);

  std::uint64_t keys = 0;
  // Key count is not known until the walk finishes, so a placeholder is
  // written and patched afterwards rather than walking the keyspace twice.
  putU64(body, 0);

  keyspace.forEachEntry([&](const std::string& key, const Entry& entry) {
    putString(body, key);
    putU64(body, static_cast<std::uint64_t>(entry.expire_at_ms));
    encodeValue(body, entry.value);
    ++keys;
  });

  for (int i = 0; i < 8; ++i) {
    body[static_cast<std::size_t>(i)] = static_cast<char>((keys >> (i * 8)) & 0xFF);
  }

  std::string image;
  image.reserve(body.size() + kMagic.size() + 4);
  image.append(kMagic);
  image.append(body);
  putU32(image, crc32(body.data(), body.size()));

  // Atomic replace: write a temp file, fsync it, then rename. A crash can
  // therefore leave the old snapshot or a stray .tmp, never a truncated image
  // that load() would accept as complete.
  const std::string temp_path = path + ".tmp";
  Result<File> temp = File::open(temp_path, File::Mode::kTruncateReadWrite);
  if (!temp.ok()) {
    return temp.status();
  }
  {
    File file = std::move(temp).value();
    BOURSE_TRY(file.writeAt(0, image.data(), image.size()));
    BOURSE_TRY(file.sync());
  }
  BOURSE_TRY(File::rename(temp_path, path));

  SnapshotStats stats;
  stats.keys = keys;
  stats.bytes = image.size();
  stats.duration_ms = static_cast<std::int64_t>(watch.elapsedMillis());
  return stats;
}

Result<SnapshotStats> Snapshot::load(cache::Keyspace& keyspace, const std::string& path) {
  const Stopwatch watch;

  Result<std::string> contents = File::readWholeFile(path);
  if (!contents.ok()) {
    return contents.status();
  }
  const std::string& image = contents.value();

  if (image.size() < kMagic.size() + 8 + 4) {
    return Status::corruption("snapshot is too small to be valid");
  }
  if (std::string_view(image).substr(0, kMagic.size()) != kMagic) {
    return Status::corruption("snapshot magic mismatch; not a Bourse snapshot or a newer format");
  }

  const std::size_t body_begin = kMagic.size();
  const std::size_t body_size = image.size() - kMagic.size() - 4;
  const std::string_view body(image.data() + body_begin, body_size);

  std::uint32_t stored_crc = 0;
  for (int i = 3; i >= 0; --i) {
    stored_crc = (stored_crc << 8) |
                 static_cast<unsigned char>(image[image.size() - 4 + static_cast<std::size_t>(i)]);
  }
  if (crc32(body.data(), body.size()) != stored_crc) {
    return Status::corruption("snapshot checksum mismatch; refusing to load a damaged image");
  }

  Reader reader(body);
  std::uint64_t declared_keys = 0;
  if (!reader.readU64(declared_keys)) {
    return Status::corruption("snapshot header is truncated");
  }

  const std::int64_t now = nowMillis();
  std::uint64_t loaded = 0;
  std::uint64_t skipped_expired = 0;

  for (std::uint64_t i = 0; i < declared_keys; ++i) {
    std::string key;
    std::uint64_t expire_raw = 0;
    if (!reader.readString(key) || !reader.readU64(expire_raw)) {
      return Status::corruption("snapshot entry " + std::to_string(i) + " is truncated");
    }

    Entry entry;
    entry.expire_at_ms = static_cast<std::int64_t>(expire_raw);
    if (!decodeValue(reader, entry.value)) {
      return Status::corruption("snapshot value for key '" + key + "' is malformed");
    }

    // TTLs are absolute, so a key whose deadline passed while the server was
    // down must not come back to life.
    if (entry.expire_at_ms != 0 && entry.expire_at_ms <= now) {
      ++skipped_expired;
      continue;
    }
    keyspace.restoreEntry(std::move(key), std::move(entry));
    ++loaded;
  }

  if (skipped_expired > 0) {
    BOURSE_LOG_INFO("snapshot: dropped ", skipped_expired, " key(s) that expired while the server was down");
  }

  SnapshotStats stats;
  stats.keys = loaded;
  stats.bytes = image.size();
  stats.duration_ms = static_cast<std::int64_t>(watch.elapsedMillis());
  return stats;
}

}  // namespace bourse::storage
