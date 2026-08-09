#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

/// \file common.hpp
/// Project-wide primitives that everything else is allowed to depend on.
/// Nothing in this header may include another Bourse header.

namespace bourse {

/// Conservative false-sharing guard.
///
/// `std::hardware_destructive_interference_size` is deliberately *not* used:
/// libstdc++ warns that its value is part of the ABI and may change between
/// GCC releases, which would silently change the layout of the lock-free
/// structures in `core/spsc_ring.hpp`. 64 bytes is correct for every CPU this
/// project targets (x86-64, ARM64 in its 64-byte-line configuration).
inline constexpr std::size_t kCacheLineSize = 64;

#if defined(__GNUC__) || defined(__clang__)
#define BOURSE_LIKELY(x) __builtin_expect(!!(x), 1)
#define BOURSE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define BOURSE_ALWAYS_INLINE inline __attribute__((always_inline))
#define BOURSE_NOINLINE __attribute__((noinline))
#else
#define BOURSE_LIKELY(x) (x)
#define BOURSE_UNLIKELY(x) (x)
#define BOURSE_ALWAYS_INLINE inline
#define BOURSE_NOINLINE
#endif

/// Inherit privately to suppress copy construction/assignment while keeping the
/// derived type movable if it opts in.
class NonCopyable {
 public:
  NonCopyable(const NonCopyable&) = delete;
  NonCopyable& operator=(const NonCopyable&) = delete;

 protected:
  NonCopyable() = default;
  ~NonCopyable() = default;
  NonCopyable(NonCopyable&&) = default;
  NonCopyable& operator=(NonCopyable&&) = default;
};

/// Generic scope guard used for cleanup paths that are not worth a bespoke RAII
/// type. Prefer a real RAII wrapper when the resource appears more than once.
template <typename Fn>
class ScopeExit {
 public:
  explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}

  ~ScopeExit() {
    if (active_) {
      fn_();
    }
  }

  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

  ScopeExit(ScopeExit&& other) noexcept : fn_(std::move(other.fn_)), active_(other.active_) {
    other.active_ = false;
  }

  ScopeExit& operator=(ScopeExit&&) = delete;

  void dismiss() noexcept { active_ = false; }

 private:
  Fn fn_;
  bool active_ = true;
};

template <typename Fn>
ScopeExit<Fn> makeScopeExit(Fn fn) {
  return ScopeExit<Fn>(std::move(fn));
}

}  // namespace bourse
