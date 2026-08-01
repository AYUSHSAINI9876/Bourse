#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "bourse/core/socket.hpp"
#include "bourse/net/connection.hpp"
#include "bourse/net/event_loop.hpp"

namespace bourse::net {

struct ServerOptions {
  std::string host = "0.0.0.0";
  std::uint16_t port = 6380;
  int backlog = 512;
  /// 0 selects hardware_concurrency. Each I/O thread owns one EventLoop and a
  /// disjoint set of connections.
  std::size_t io_threads = 0;
  std::string name = "bourse";
};

/// Multi-reactor TCP server: one acceptor loop plus N I/O loops.
///
/// Three designs were on the table:
///
///   * **Thread per connection.** Simple, but 10k clients means 10k stacks and
///     a scheduler that spends its time context-switching.
///   * **Single reactor.** No locking anywhere, but one core is the ceiling.
///   * **Acceptor + N reactors** (this one). Each connection is bound to one
///     loop for its whole life, so per-connection state still needs no locking,
///     and throughput scales with cores.
///
/// The only shared structure is the connection table, which is touched on
/// accept, on close, and on cross-loop delivery (pub/sub and market data) --
/// never on the per-request path.
class TcpServer {
 public:
  /// Produces a codec for each accepted connection. A factory rather than a
  /// shared instance because codecs may hold per-connection parse state.
  using CodecFactory = std::function<CodecPtr()>;

  TcpServer(ServerOptions options, CodecFactory codec_factory);
  ~TcpServer();

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  /// Binds, listens, and spins up the I/O threads. Does not block.
  Status start();

  /// Runs the acceptor loop on the calling thread until `stop()`.
  void runForever();

  void stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }
  [[nodiscard]] std::size_t connectionCount() const;
  [[nodiscard]] EventLoop& acceptorLoop() noexcept { return acceptor_loop_; }
  [[nodiscard]] std::string_view pollerName() const noexcept { return acceptor_loop_.pollerName(); }

  /// Delivers `data` to one connection, hopping to its owning loop first.
  /// Silently does nothing if the connection has since closed -- which is the
  /// entire reason pub/sub addresses subscribers by id rather than by pointer.
  void sendTo(std::uint64_t connection_id, std::string data);

  /// Delivers to every connection. Used for the market-data fan-out.
  void broadcast(std::string data);

  /// Runs `fn` on every live connection, on the caller's thread. Intended for
  /// introspection (CLIENT LIST), not for I/O.
  void inspectConnections(const std::function<void(const ConnectionPtr&)>& fn) const;

 private:
  void handleAcceptReady();
  EventLoop& nextLoop();
  void removeConnection(const ConnectionPtr& connection);

  ServerOptions options_;
  CodecFactory codec_factory_;

  Socket listener_;
  EventLoop acceptor_loop_;
  std::uint64_t listener_token_ = 0;
  std::uint16_t bound_port_ = 0;

  std::vector<std::unique_ptr<EventLoopThread>> io_threads_;
  std::atomic<std::size_t> next_loop_{0};

  mutable std::mutex connections_mutex_;
  std::unordered_map<std::uint64_t, ConnectionPtr> connections_;
  std::unordered_map<std::uint64_t, EventLoop*> connection_loops_;
  std::atomic<std::uint64_t> next_connection_id_{1};

  std::atomic<bool> started_{false};
};

}  // namespace bourse::net
