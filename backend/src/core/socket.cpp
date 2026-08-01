#include "bourse/core/socket.hpp"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <sstream>
#include <utility>

#if defined(BOURSE_PLATFORM_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_arg_t = int;
#define BOURSE_CLOSESOCKET ::closesocket
#else
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen_arg_t = socklen_t;
#define BOURSE_CLOSESOCKET ::close
#endif

namespace bourse {
namespace {

std::string addressToString(const sockaddr_storage& storage) {
  char host[NI_MAXHOST] = {};
  char service[NI_MAXSERV] = {};
  const auto* addr = reinterpret_cast<const sockaddr*>(&storage);
  const socklen_arg_t len = storage.ss_family == AF_INET6 ? static_cast<socklen_arg_t>(sizeof(sockaddr_in6))
                                                          : static_cast<socklen_arg_t>(sizeof(sockaddr_in));
  if (::getnameinfo(addr, len, host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return "<unknown>";
  }
  std::ostringstream oss;
  oss << host << ':' << service;
  return oss.str();
}

IoOutcome classifyError(int code) {
#if defined(BOURSE_PLATFORM_WINDOWS)
  switch (code) {
    case WSAEWOULDBLOCK: return IoOutcome::kWouldBlock;
    case WSAEINTR: return IoOutcome::kInterrupted;
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAESHUTDOWN: return IoOutcome::kClosed;
    default: return IoOutcome::kError;
  }
#else
  switch (code) {
    case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
      return IoOutcome::kWouldBlock;
    case EINTR: return IoOutcome::kInterrupted;
    case ECONNRESET:
    case EPIPE:
    case ECONNABORTED: return IoOutcome::kClosed;
    default: return IoOutcome::kError;
  }
#endif
}

}  // namespace

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalidSocket; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidSocket;
  }
  return *this;
}

void Socket::close() noexcept {
  if (handle_ != kInvalidSocket) {
    BOURSE_CLOSESOCKET(handle_);
    handle_ = kInvalidSocket;
  }
}

SocketHandle Socket::release() noexcept {
  SocketHandle released = handle_;
  handle_ = kInvalidSocket;
  return released;
}

int Socket::lastError() noexcept {
#if defined(BOURSE_PLATFORM_WINDOWS)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

std::string Socket::describeError(int code) {
#if defined(BOURSE_PLATFORM_WINDOWS)
  char* text = nullptr;
  ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<char*>(&text), 0, nullptr);
  std::string message = text != nullptr ? text : "unknown";
  if (text != nullptr) {
    ::LocalFree(text);
  }
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
    message.pop_back();
  }
  return message;
#else
  return std::strerror(code);
#endif
}

Status Socket::initializeNetworking() {
  static std::once_flag once;
  static Status result = Status::success();
  std::call_once(once, [] {
#if defined(BOURSE_PLATFORM_WINDOWS)
    WSADATA data{};
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
      result = Status::ioError("WSAStartup failed: " + describeError(rc));
    }
#else
    // A client disconnecting mid-write would otherwise deliver SIGPIPE and kill
    // the process. Ignoring it turns the same event into EPIPE on write(), which
    // the connection state machine already handles as a normal close.
    std::signal(SIGPIPE, SIG_IGN);
#endif
  });
  return result;
}

Result<Socket> Socket::createTcpListener(const std::string& host, std::uint16_t port, int backlog) {
  BOURSE_TRY(initializeNetworking());

  addrinfo hints{};
  hints.ai_family = AF_INET;  // IPv4 keeps the address handling in the event loop simple
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  const std::string port_text = std::to_string(port);
  addrinfo* resolved = nullptr;
  const int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), port_text.c_str(), &hints, &resolved);
  if (rc != 0 || resolved == nullptr) {
    return Status::ioError("getaddrinfo(" + host + ":" + port_text + ") failed");
  }

  Socket sock(static_cast<SocketHandle>(::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol)));
  if (!sock.valid()) {
    ::freeaddrinfo(resolved);
    return Status::ioError("socket() failed: " + describeError(lastError()));
  }

  BOURSE_TRY(sock.setReuseAddress(true));

  if (::bind(sock.handle_, resolved->ai_addr, static_cast<socklen_arg_t>(resolved->ai_addrlen)) != 0) {
    const int err = lastError();
    ::freeaddrinfo(resolved);
    return Status::ioError("bind(" + host + ":" + port_text + ") failed: " + describeError(err));
  }
  ::freeaddrinfo(resolved);

  if (::listen(sock.handle_, backlog) != 0) {
    return Status::ioError("listen() failed: " + describeError(lastError()));
  }

  BOURSE_TRY(sock.setNonBlocking(true));
  return sock;
}

Result<Socket> Socket::connectTcp(const std::string& host, std::uint16_t port, int /*timeout_ms*/) {
  BOURSE_TRY(initializeNetworking());

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  const std::string port_text = std::to_string(port);
  addrinfo* resolved = nullptr;
  if (::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
    return Status::ioError("getaddrinfo(" + host + ":" + port_text + ") failed");
  }

  Socket sock(static_cast<SocketHandle>(::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol)));
  if (!sock.valid()) {
    ::freeaddrinfo(resolved);
    return Status::ioError("socket() failed: " + describeError(lastError()));
  }

  const int rc = ::connect(sock.handle_, resolved->ai_addr, static_cast<socklen_arg_t>(resolved->ai_addrlen));
  const int err = lastError();
  ::freeaddrinfo(resolved);
  if (rc != 0) {
    return Status::ioError("connect(" + host + ":" + port_text + ") failed: " + describeError(err));
  }
  return sock;
}

Result<Socket> Socket::accept(std::string* peer_address) {
  sockaddr_storage peer{};
  socklen_arg_t peer_len = sizeof(peer);

#if defined(__linux__)
  // accept4 applies O_NONBLOCK and FD_CLOEXEC atomically. Doing it as
  // accept() + fcntl() leaves a window in which a concurrent fork()/exec()
  // would leak the descriptor into the child.
  const auto accepted = static_cast<SocketHandle>(
      ::accept4(handle_, reinterpret_cast<sockaddr*>(&peer), &peer_len, SOCK_NONBLOCK | SOCK_CLOEXEC));
#else
  const auto accepted = static_cast<SocketHandle>(::accept(handle_, reinterpret_cast<sockaddr*>(&peer), &peer_len));
#endif

  if (accepted == kInvalidSocket) {
    const int err = lastError();
    const IoOutcome outcome = classifyError(err);
    if (outcome == IoOutcome::kWouldBlock || outcome == IoOutcome::kInterrupted) {
      return Status(ErrorCode::kWouldBlock, "no pending connection");
    }
    return Status::ioError("accept() failed: " + describeError(err));
  }

  Socket accepted_socket(accepted);
#if !defined(__linux__)
  BOURSE_TRY(accepted_socket.setNonBlocking(true));
#endif
  if (peer_address != nullptr) {
    *peer_address = addressToString(peer);
  }
  return accepted_socket;
}

Status Socket::setNonBlocking(bool enable) {
  if (!valid()) {
    return Status::ioError("setNonBlocking on an invalid socket");
  }
#if defined(BOURSE_PLATFORM_WINDOWS)
  u_long mode = enable ? 1 : 0;
  if (::ioctlsocket(handle_, FIONBIO, &mode) != 0) {
    return Status::ioError("ioctlsocket(FIONBIO): " + describeError(lastError()));
  }
#else
  const int flags = ::fcntl(handle_, F_GETFL, 0);
  if (flags < 0) {
    return Status::ioError("fcntl(F_GETFL): " + describeError(errno));
  }
  const int updated = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (::fcntl(handle_, F_SETFL, updated) < 0) {
    return Status::ioError("fcntl(F_SETFL): " + describeError(errno));
  }
#endif
  return Status::success();
}

namespace {
Status setBoolOption(SocketHandle handle, int level, int option, bool enable, const char* name) {
  const int value = enable ? 1 : 0;
  if (::setsockopt(handle, level, option, reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
    return Status::ioError(std::string("setsockopt(") + name + "): " + Socket::describeError(Socket::lastError()));
  }
  return Status::success();
}
}  // namespace

Status Socket::setTcpNoDelay(bool enable) {
  // Disables Nagle. Without this, a small RESP reply waits for the delayed-ACK
  // timer before leaving the box, which shows up as a ~40ms p99 cliff.
  return setBoolOption(handle_, IPPROTO_TCP, TCP_NODELAY, enable, "TCP_NODELAY");
}

Status Socket::setReuseAddress(bool enable) {
  return setBoolOption(handle_, SOL_SOCKET, SO_REUSEADDR, enable, "SO_REUSEADDR");
}

Status Socket::setKeepAlive(bool enable) {
  return setBoolOption(handle_, SOL_SOCKET, SO_KEEPALIVE, enable, "SO_KEEPALIVE");
}

Status Socket::setSendBufferSize(int bytes) {
  if (::setsockopt(handle_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes)) != 0) {
    return Status::ioError("setsockopt(SO_SNDBUF): " + describeError(lastError()));
  }
  return Status::success();
}

Status Socket::setReceiveBufferSize(int bytes) {
  if (::setsockopt(handle_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes)) != 0) {
    return Status::ioError("setsockopt(SO_RCVBUF): " + describeError(lastError()));
  }
  return Status::success();
}

IoResult Socket::read(void* buffer, std::size_t n) const {
  IoResult result;
  if (!valid()) {
    result.outcome = IoOutcome::kError;
    return result;
  }
#if defined(BOURSE_PLATFORM_WINDOWS)
  const int got = ::recv(handle_, static_cast<char*>(buffer), static_cast<int>(n), 0);
#else
  const ssize_t got = ::recv(handle_, buffer, n, 0);
#endif
  if (got > 0) {
    result.outcome = IoOutcome::kOk;
    result.bytes = static_cast<std::size_t>(got);
    return result;
  }
  if (got == 0) {
    result.outcome = IoOutcome::kClosed;  // orderly shutdown by the peer
    return result;
  }
  result.error_code = lastError();
  result.outcome = classifyError(result.error_code);
  return result;
}

IoResult Socket::write(const void* buffer, std::size_t n) const {
  IoResult result;
  if (!valid()) {
    result.outcome = IoOutcome::kError;
    return result;
  }
#if defined(BOURSE_PLATFORM_WINDOWS)
  const int put = ::send(handle_, static_cast<const char*>(buffer), static_cast<int>(n), 0);
#elif defined(MSG_NOSIGNAL)
  const ssize_t put = ::send(handle_, buffer, n, MSG_NOSIGNAL);
#else
  const ssize_t put = ::send(handle_, buffer, n, 0);
#endif
  if (put >= 0) {
    result.outcome = IoOutcome::kOk;
    result.bytes = static_cast<std::size_t>(put);
    return result;
  }
  result.error_code = lastError();
  result.outcome = classifyError(result.error_code);
  return result;
}

Status Socket::shutdownWrite() {
  if (!valid()) {
    return Status::success();
  }
#if defined(BOURSE_PLATFORM_WINDOWS)
  ::shutdown(handle_, SD_SEND);
#else
  ::shutdown(handle_, SHUT_WR);
#endif
  return Status::success();
}

int Socket::takeSocketError() const {
  int value = 0;
  socklen_arg_t len = sizeof(value);
  if (::getsockopt(handle_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &len) != 0) {
    return lastError();
  }
  return value;
}

std::string Socket::localAddress() const {
  sockaddr_storage storage{};
  socklen_arg_t len = sizeof(storage);
  if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&storage), &len) != 0) {
    return "<unbound>";
  }
  return addressToString(storage);
}

std::uint16_t Socket::localPort() const {
  sockaddr_storage storage{};
  socklen_arg_t len = sizeof(storage);
  if (::getsockname(handle_, reinterpret_cast<sockaddr*>(&storage), &len) != 0) {
    return 0;
  }
  if (storage.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
  }
  return ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
}

}  // namespace bourse
