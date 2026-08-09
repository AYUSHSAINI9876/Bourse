#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "bourse/core/common.hpp"

namespace bourse {

/// Chunked free-list allocator for a single object type.
///
/// The matching engine allocates and frees `Order` objects at the same rate as
/// order entry. Routing that through the global allocator puts a lock (or at
/// best a thread-cache lookup) plus unpredictable latency in the middle of the
/// hot path, and it fragments. This pool pre-allocates in geometric chunks,
/// recycles through an intrusive-free-list-of-pointers, and never returns
/// memory to the OS during a session -- steady-state order entry performs zero
/// calls to operator new.
///
/// Not thread-safe by design: each pool is owned by one matching-engine thread.
template <typename T>
class ObjectPool {
 public:
  explicit ObjectPool(std::size_t initial_capacity = 1024)
      : next_chunk_objects_(initial_capacity == 0 ? 1 : initial_capacity) {
    growChunk();
  }

  ~ObjectPool() = default;

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;
  ObjectPool(ObjectPool&&) noexcept = default;
  ObjectPool& operator=(ObjectPool&&) noexcept = default;

  template <typename... Args>
  [[nodiscard]] T* acquire(Args&&... args) {
    if (BOURSE_UNLIKELY(free_list_.empty())) {
      growChunk();
    }
    T* slot = free_list_.back();
    free_list_.pop_back();
    ++live_;
    return ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
  }

  void release(T* obj) noexcept {
    if (obj == nullptr) {
      return;
    }
    obj->~T();
    free_list_.push_back(obj);
    --live_;
  }

  [[nodiscard]] std::size_t live() const noexcept { return live_; }

  [[nodiscard]] std::size_t available() const noexcept { return free_list_.size(); }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  /// Number of times the pool had to ask the OS for memory. In a well-sized
  /// pool this stops increasing after warm-up; the benchmark asserts on it.
  [[nodiscard]] std::size_t chunkCount() const noexcept { return chunks_.size(); }

 private:
  struct Storage {
    alignas(alignof(T)) unsigned char bytes[sizeof(T)];
  };

  void growChunk() {
    const std::size_t n = next_chunk_objects_;
    auto chunk = std::make_unique<Storage[]>(n);
    free_list_.reserve(free_list_.size() + n);
    for (std::size_t i = n; i-- > 0;) {
      free_list_.push_back(reinterpret_cast<T*>(&chunk[i]));
    }
    chunks_.push_back(std::move(chunk));
    capacity_ += n;
    next_chunk_objects_ = n * 2;  // geometric growth bounds the number of chunks
  }

  std::vector<std::unique_ptr<Storage[]>> chunks_;
  std::vector<T*> free_list_;
  std::size_t next_chunk_objects_;
  std::size_t capacity_ = 0;
  std::size_t live_ = 0;
};

/// Bump allocator for objects that are all freed at once (a parsed AST, a
/// request's scratch space). Allocation is a pointer increment; deallocation is
/// a no-op until `reset()` reclaims everything.
class Arena {
 public:
  static constexpr std::size_t kDefaultBlockSize = 64 * 1024;

  explicit Arena(std::size_t block_size = kDefaultBlockSize)
      : block_size_(block_size == 0 ? kDefaultBlockSize : block_size) {}

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
    const std::size_t misalign = reinterpret_cast<std::uintptr_t>(cursor_) % alignment;
    const std::size_t padding = misalign == 0 ? 0 : alignment - misalign;
    if (cursor_ == nullptr || padding + bytes > remaining_) {
      newBlock(bytes + alignment);
      return allocate(bytes, alignment);
    }
    cursor_ += padding;
    remaining_ -= padding;
    void* result = cursor_;
    cursor_ += bytes;
    remaining_ -= bytes;
    used_ += bytes;
    return result;
  }

  template <typename T, typename... Args>
  [[nodiscard]] T* create(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "Arena does not run destructors; use ObjectPool for non-trivial types");
    return ::new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
  }

  void reset() noexcept {
    blocks_.clear();
    cursor_ = nullptr;
    remaining_ = 0;
    used_ = 0;
  }

  [[nodiscard]] std::size_t bytesUsed() const noexcept { return used_; }

  [[nodiscard]] std::size_t blockCount() const noexcept { return blocks_.size(); }

 private:
  void newBlock(std::size_t min_size) {
    const std::size_t size = min_size > block_size_ ? min_size : block_size_;
    blocks_.push_back(std::make_unique<unsigned char[]>(size));
    cursor_ = blocks_.back().get();
    remaining_ = size;
  }

  std::vector<std::unique_ptr<unsigned char[]>> blocks_;
  unsigned char* cursor_ = nullptr;
  std::size_t remaining_ = 0;
  std::size_t block_size_;
  std::size_t used_ = 0;
};

}  // namespace bourse
