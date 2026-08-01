#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "bourse/core/result.hpp"

namespace bourse {

#if defined(BOURSE_PLATFORM_WINDOWS)
using SocketHandle = std::uintptr_t;
inline constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~0ULL);
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

/// Outcome of a non-blocking read or write.
///
/// A plain `Result<size_t>` would force the event loop to inspect an error
/// string to distinguish "no data right now" (normal, re-arm and return) from
/// "peer closed" (tear down) from "real error" (log and tear down). Making
/// those three states part of the type removes an entire class of bug from the
/// connection state machine.
enum class IoOutcome : std::uint8_t {
  kOk,
  kWouldBlock,
  kClosed,
  kInterrupted,
  kError,
};

struct IoResult {
  IoOutcome outcome = IoOutcome::kOk;
  std::size_t bytes = 0;
  int error_code = 0;

  [[nodiscard]] bool ok() const noexcept { return outcome == IoOutcome::kOk; }
  [[nodiscard]] bool retryable() const noexcept {
    return outcome == IoOutcome::kWouldBlock || outcome == IoOutcome::kInterrupted;
  }
};

/// Move-only RAII TCP socket.
class Socket {
 public:
  Socket() = default;
  explicit Socket(SocketHandle handle) noexcept : handle_(handle) {}
  ~Socket();

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  /// One-time process setup. No-op on POSIX; WSAStartup on Windows. Safe to
  /// call more than once.
  static Status initializeNetworking();

  static Result<Socket> createTcpListener(const std::string& host, std::uint16_t port, int backlog = 512);
  static Result<Socket> connectTcp(const std::string& host, std::uint16_t port, int timeout_ms = 5000);

  /// Accepts one pending connection.
  ///
  /// An empty backlog is reported as `ErrorCode::kWouldBlock`, not as an error:
  /// that is the normal termination condition of the acceptor's drain loop, and
  /// making it a distinct code stops the event loop from logging it as a
  /// failure on every single iteration.
  [[nodiscard]] Result<Socket> accept(std::string* peer_address = nullptr);

  Status setNonBlocking(bool enable);
  Status setTcpNoDelay(bool enable);
  Status setReuseAddress(bool enable);
  Status setKeepAlive(bool enable);
  Status setSendBufferSize(int bytes);
  Status setReceiveBufferSize(int bytes);

  [[nodiscard]] IoResult read(void* buffer, std::size_t n) const;
  [[nodiscard]] IoResult write(const void* buffer, std::size_t n) const;

  Status shutdownWrite();
  void close() noexcept;
  [[nodiscard]] SocketHandle release() noexcept;

  [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidSocket; }
  [[nodiscard]] SocketHandle handle() const noexcept { return handle_; }

  /// Reads and clears SO_ERROR. Used to complete a non-blocking connect.
  [[nodiscard]] int takeSocketError() const;

  /// "127.0.0.1:54321" for the local end of a connected socket.
  [[nodiscard]] std::string localAddress() const;
  [[nodiscard]] std::uint16_t localPort() const;

  /// Human-readable description of an errno / WSAGetLastError value.
  [[nodiscard]] static std::string describeError(int code);
  [[nodiscard]] static int lastError() noexcept;

 private:
  SocketHandle handle_ = kInvalidSocket;
};

}  // namespace bourse
