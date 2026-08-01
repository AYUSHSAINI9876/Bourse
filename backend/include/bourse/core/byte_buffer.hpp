#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace bourse {

/// A growable byte buffer with independent read and write cursors.
///
/// This is the single most performance-relevant data structure in the network
/// layer, so the design is deliberate:
///
///   [ prependable | readable | writable ]
///     ^            ^          ^
///     0            read_      write_
///
///   * Reads never memmove. `retrieve()` only advances `read_`.
///   * `ensureWritable()` first tries to reclaim the prependable region by
///     compacting, and only reallocates when compaction is insufficient. A
///     long-lived connection therefore reaches a steady state and stops
///     allocating entirely.
///   * `view()` hands out a `std::string_view` over the readable region so the
///     RESP and HTTP parsers can tokenise without copying. This is exactly the
///     std::string -> std::string_view change documented in docs/benchmarks.md.
///
/// Not thread-safe: each connection owns its own buffers and is driven by a
/// single event-loop thread.
class ByteBuffer {
 public:
  static constexpr std::size_t kDefaultPrepend = 8;
  static constexpr std::size_t kDefaultCapacity = 1024;

  explicit ByteBuffer(std::size_t initial_capacity = kDefaultCapacity)
      : storage_(kDefaultPrepend + initial_capacity), read_(kDefaultPrepend), write_(kDefaultPrepend) {}

  [[nodiscard]] std::size_t readable() const noexcept { return write_ - read_; }
  [[nodiscard]] std::size_t writable() const noexcept { return storage_.size() - write_; }
  [[nodiscard]] std::size_t prependable() const noexcept { return read_; }
  [[nodiscard]] bool empty() const noexcept { return readable() == 0; }
  [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

  [[nodiscard]] const char* peek() const noexcept { return storage_.data() + read_; }
  [[nodiscard]] char* beginWrite() noexcept { return storage_.data() + write_; }
  [[nodiscard]] const char* beginWrite() const noexcept { return storage_.data() + write_; }

  /// Zero-copy view of the unread region. Invalidated by any mutating call.
  [[nodiscard]] std::string_view view() const noexcept { return {peek(), readable()}; }

  void append(const char* data, std::size_t len);
  void append(std::string_view data) { append(data.data(), data.size()); }
  void appendByte(char c) { append(&c, 1); }

  void ensureWritable(std::size_t len);
  void hasWritten(std::size_t len) noexcept { write_ += len; }
  void unwrite(std::size_t len) noexcept { write_ -= len; }

  void retrieve(std::size_t len) noexcept;
  void retrieveAll() noexcept;
  [[nodiscard]] std::string retrieveAsString(std::size_t len);
  [[nodiscard]] std::string retrieveAllAsString() { return retrieveAsString(readable()); }

  /// Returns the offset of `needle` within the readable region, or npos.
  [[nodiscard]] std::size_t find(std::string_view needle, std::size_t from = 0) const noexcept;
  [[nodiscard]] std::size_t findCRLF(std::size_t from = 0) const noexcept { return find("\r\n", from); }

  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  /// Writes `len` bytes immediately in front of the readable region without
  /// moving it. Used to back-fill a length header once the body size is known.
  /// Returns false when the prependable region is too small.
  bool prepend(const void* data, std::size_t len) noexcept;

  /// Releases excess capacity, keeping `reserve` writable bytes. Called when a
  /// connection's buffer has ballooned from one large request.
  void shrink(std::size_t reserve);

  void swap(ByteBuffer& other) noexcept {
    storage_.swap(other.storage_);
    std::swap(read_, other.read_);
    std::swap(write_, other.write_);
  }

 private:
  void makeSpace(std::size_t len);

  std::vector<char> storage_;
  std::size_t read_;
  std::size_t write_;
};

}  // namespace bourse
