#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

#include "bourse/core/common.hpp"

namespace bourse {

/// Wait-free single-producer / single-consumer ring buffer.
///
/// This is the hand-off between the network thread and the matching-engine
/// thread. It is the reason order entry never takes a lock.
///
/// Two design points worth defending in review:
///
///  1. **Separate cache lines for the two cursors.** The producer writes
///     `write_`, the consumer writes `read_`. If they shared a line, every push
///     would invalidate the consumer's copy and vice versa -- textbook false
///     sharing, and it costs roughly 4x throughput on x86.
///
///  2. **Cached opposite cursor.** The producer keeps `cached_read_` in its own
///     line and only re-loads the real `read_` when it *appears* full. In
///     steady state where the queue is not near-full, the producer never reads
///     the consumer's cache line at all.
///
/// Capacity must be a power of two so the modulo becomes a mask. One slot is
/// left unused to disambiguate full from empty without a separate counter.
template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2, "SpscRing needs at least two slots");
  static_assert((Capacity & (Capacity - 1)) == 0, "SpscRing capacity must be a power of two");
  static_assert(std::is_nothrow_move_assignable_v<T> || std::is_copy_assignable_v<T>,
                "SpscRing element must be assignable");

 public:
  static constexpr std::size_t kCapacity = Capacity;
  static constexpr std::size_t kMask = Capacity - 1;

  SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  /// Producer side. Returns false when the ring is full -- callers decide
  /// whether that means backpressure or a dropped message.
  template <typename U>
  bool push(U&& value) {
    const std::size_t w = write_.idx.load(std::memory_order_relaxed);
    const std::size_t next = (w + 1) & kMask;
    if (next == write_.cached_other) {
      write_.cached_other = read_.idx.load(std::memory_order_acquire);
      if (next == write_.cached_other) {
        return false;  // genuinely full
      }
    }
    slots_[w] = std::forward<U>(value);
    write_.idx.store(next, std::memory_order_release);
    return true;
  }

  /// Consumer side. Returns false when the ring is empty.
  bool pop(T& out) {
    const std::size_t r = read_.idx.load(std::memory_order_relaxed);
    if (r == read_.cached_other) {
      read_.cached_other = write_.idx.load(std::memory_order_acquire);
      if (r == read_.cached_other) {
        return false;  // genuinely empty
      }
    }
    out = std::move(slots_[r]);
    read_.idx.store((r + 1) & kMask, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::optional<T> pop() {
    T tmp{};
    if (!pop(tmp)) {
      return std::nullopt;
    }
    return std::optional<T>{std::move(tmp)};
  }

  /// Approximate -- both cursors are read without synchronisation, so the
  /// result is only a hint. Safe to call from either side for metrics.
  [[nodiscard]] std::size_t sizeApprox() const noexcept {
    const std::size_t w = write_.idx.load(std::memory_order_acquire);
    const std::size_t r = read_.idx.load(std::memory_order_acquire);
    return (w - r) & kMask;
  }

  [[nodiscard]] bool emptyApprox() const noexcept {
    return write_.idx.load(std::memory_order_acquire) == read_.idx.load(std::memory_order_acquire);
  }

 private:
  struct alignas(kCacheLineSize) Cursor {
    std::atomic<std::size_t> idx{0};
    std::size_t cached_other{0};
  };

  Cursor write_;
  Cursor read_;
  alignas(kCacheLineSize) std::array<T, Capacity> slots_{};
};

}  // namespace bourse
