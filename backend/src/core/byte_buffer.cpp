#include "bourse/core/byte_buffer.hpp"

#include <algorithm>
#include <cassert>

namespace bourse {

void ByteBuffer::append(const char* data, std::size_t len) {
  if (len == 0) {
    return;
  }
  ensureWritable(len);
  std::memcpy(beginWrite(), data, len);
  hasWritten(len);
}

void ByteBuffer::ensureWritable(std::size_t len) {
  if (writable() < len) {
    makeSpace(len);
  }
  assert(writable() >= len);
}

void ByteBuffer::makeSpace(std::size_t len) {
  // Two strategies, in order of cost:
  //
  //  1. Compact. Once a connection has consumed some bytes, `read_` has drifted
  //     forward and left a hole at the front. Sliding the unread bytes back is
  //     one memmove of the *unread* region -- typically a partial frame, so a
  //     few dozen bytes -- and it avoids an allocation entirely. This is what
  //     lets a long-lived connection reach a steady state.
  //
  //  2. Grow. Only when compaction still cannot satisfy the request. Growth is
  //     geometric so repeated appends stay amortised O(1).
  if (prependable() + writable() >= len + kDefaultPrepend) {
    const std::size_t readable_bytes = readable();
    if (readable_bytes > 0) {
      std::memmove(storage_.data() + kDefaultPrepend, storage_.data() + read_, readable_bytes);
    }
    read_ = kDefaultPrepend;
    write_ = read_ + readable_bytes;
    return;
  }

  const std::size_t required = write_ + len;
  std::size_t new_capacity = storage_.empty() ? kDefaultCapacity : storage_.size();
  while (new_capacity < required) {
    new_capacity *= 2;
  }
  storage_.resize(new_capacity);
}

void ByteBuffer::retrieve(std::size_t len) noexcept {
  if (len >= readable()) {
    retrieveAll();
    return;
  }
  read_ += len;
}

void ByteBuffer::retrieveAll() noexcept {
  read_ = kDefaultPrepend;
  write_ = kDefaultPrepend;
}

std::string ByteBuffer::retrieveAsString(std::size_t len) {
  len = std::min(len, readable());
  std::string result(peek(), len);
  retrieve(len);
  return result;
}

std::size_t ByteBuffer::find(std::string_view needle, std::size_t from) const noexcept {
  if (needle.empty()) {
    return from <= readable() ? from : npos;
  }
  const std::size_t available = readable();
  if (from >= available || needle.size() > available - from) {
    return npos;
  }
  const std::string_view haystack(peek() + from, available - from);
  const std::size_t pos = haystack.find(needle);
  return pos == std::string_view::npos ? npos : pos + from;
}

bool ByteBuffer::prepend(const void* data, std::size_t len) noexcept {
  if (len > prependable()) {
    return false;
  }
  read_ -= len;
  std::memcpy(storage_.data() + read_, data, len);
  return true;
}

void ByteBuffer::shrink(std::size_t reserve) {
  const std::size_t readable_bytes = readable();
  ByteBuffer compact(readable_bytes + reserve);
  compact.append(peek(), readable_bytes);
  swap(compact);
}

}  // namespace bourse
