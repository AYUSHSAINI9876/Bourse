#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "bourse/core/result.hpp"

namespace bourse::net {

/// Interest / readiness flags. Deliberately a plain bitmask rather than the
/// platform's own constants so the rest of the codebase never includes
/// <sys/epoll.h>.
enum EventFlags : std::uint32_t {
  kNone = 0u,
  kReadable = 1u << 0,
  kWritable = 1u << 1,
  kError = 1u << 2,
  kHangup = 1u << 3,
};

struct PollEvent {
  int fd = -1;
  std::uint32_t flags = kNone;
  /// Opaque value supplied at registration. A 64-bit token rather than a
  /// pointer: a pointer registered with the kernel outlives the C++ object if a
  /// connection is destroyed between the epoll_wait return and the dispatch,
  /// and dereferencing it is then a use-after-free. An integer id forces the
  /// dispatcher to re-validate through the connection table.
  std::uint64_t token = 0;
};

/// Readiness-notification abstraction.
///
/// Two implementations ship: `epoll` on Linux (O(1) per ready descriptor, the
/// production path) and `poll` everywhere else (O(n) per call, portable
/// fallback that keeps the project buildable and testable off-Linux). The
/// interface is what lets docs/benchmarks.md compare them directly on the same
/// workload rather than arguing about it in the abstract.
class Poller {
 public:
  virtual ~Poller() = default;

  virtual Status add(int fd, std::uint32_t interest, std::uint64_t token) = 0;
  virtual Status modify(int fd, std::uint32_t interest, std::uint64_t token) = 0;
  virtual Status remove(int fd) = 0;

  /// Blocks up to `timeout_ms` (-1 = indefinitely). Fills `out` with ready
  /// descriptors and returns how many, or a negative value on error.
  virtual int wait(std::vector<PollEvent>& out, int timeout_ms) = 0;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual std::size_t registeredCount() const noexcept = 0;

  /// Picks the best available implementation for the platform.
  static std::unique_ptr<Poller> create();
  /// Forces the portable implementation; used by the benchmark comparison.
  static std::unique_ptr<Poller> createPortable();
};

}  // namespace bourse::net
