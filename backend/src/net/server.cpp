#include "bourse/net/server.hpp"

#include <thread>
#include <utility>

#include "bourse/core/logger.hpp"
#include "bourse/core/metrics.hpp"

namespace bourse::net {

TcpServer::TcpServer(ServerOptions options, CodecFactory codec_factory)
    : options_(std::move(options)), codec_factory_(std::move(codec_factory)) {}

TcpServer::~TcpServer() { stop(); }

Status TcpServer::start() {
  if (started_.exchange(true, std::memory_order_acq_rel)) {
    return Status::alreadyExists("server already started");
  }

  Result<Socket> listener = Socket::createTcpListener(options_.host, options_.port, options_.backlog);
  if (!listener.ok()) {
    started_.store(false, std::memory_order_release);
    return listener.status();
  }
  listener_ = std::move(listener).value();
  bound_port_ = listener_.localPort();

  std::size_t thread_count = options_.io_threads;
  if (thread_count == 0) {
    const unsigned hardware = std::thread::hardware_concurrency();
    thread_count = hardware == 0 ? 2 : static_cast<std::size_t>(hardware);
  }
  io_threads_.reserve(thread_count);
  for (std::size_t i = 0; i < thread_count; ++i) {
    io_threads_.push_back(std::make_unique<EventLoopThread>(options_.name + "-io-" + std::to_string(i)));
  }

  Result<std::uint64_t> token = acceptor_loop_.registerFd(static_cast<int>(listener_.handle()), kReadable,
                                                          [this](std::uint32_t) { handleAcceptReady(); });
  if (!token.ok()) {
    started_.store(false, std::memory_order_release);
    return token.status();
  }
  listener_token_ = token.value();

  BOURSE_LOG_INFO(options_.name, " listening on ", options_.host, ':', bound_port_, " (poller=",
                  acceptor_loop_.pollerName(), ", io_threads=", thread_count, ')');
  return Status::success();
}

void TcpServer::handleAcceptReady() {
  // Drain the backlog in a loop. Accepting one connection per readiness event
  // would cap the accept rate at one per epoll_wait return, which collapses
  // under a connection storm.
  for (;;) {
    std::string peer;
    Result<Socket> accepted = listener_.accept(&peer);

    if (!accepted.ok()) {
      if (accepted.status().code() == ErrorCode::kWouldBlock) {
        return;  // backlog drained -- the normal exit
      }
      BOURSE_LOG_WARN("accept failed: ", accepted.status().toString());
      return;
    }

    const std::uint64_t id = next_connection_id_.fetch_add(1, std::memory_order_relaxed);
    EventLoop& loop = nextLoop();

    auto connection = std::make_shared<Connection>(loop, std::move(accepted).value(), std::move(peer), id,
                                                   codec_factory_());
    connection->setCloseCallback([this](const ConnectionPtr& closed) { removeConnection(closed); });

    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      connections_.emplace(id, connection);
      connection_loops_.emplace(id, &loop);
    }

    // The connection must be registered with its poller on its own loop
    // thread; doing it here would race with that thread's dispatch.
    loop.post([connection] {
      const Status status = connection->start();
      if (!status.ok()) {
        BOURSE_LOG_WARN("failed to start connection ", connection->id(), ": ", status.toString());
        connection->closeNow();
      }
    });
  }
}

EventLoop& TcpServer::nextLoop() {
  if (io_threads_.empty()) {
    return acceptor_loop_;
  }
  // Round-robin. Least-connections would balance better under long-lived
  // asymmetric sessions, but it needs a shared counter on the accept path and
  // round-robin is within noise for this workload.
  const std::size_t index = next_loop_.fetch_add(1, std::memory_order_relaxed) % io_threads_.size();
  return io_threads_[index]->loop();
}

void TcpServer::removeConnection(const ConnectionPtr& connection) {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  connections_.erase(connection->id());
  connection_loops_.erase(connection->id());
}

void TcpServer::sendTo(std::uint64_t connection_id, std::string data) {
  ConnectionPtr target;
  EventLoop* loop = nullptr;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto connection_it = connections_.find(connection_id);
    auto loop_it = connection_loops_.find(connection_id);
    if (connection_it == connections_.end() || loop_it == connection_loops_.end()) {
      return;  // already gone
    }
    target = connection_it->second;
    loop = loop_it->second;
  }

  // Hop to the owning loop. Writing from the publisher's thread would race with
  // that loop's own writes to the same output buffer.
  loop->post([target, payload = std::move(data)] {
    if (!target->closed()) {
      target->send(payload);
    }
  });
}

void TcpServer::broadcast(std::string data) {
  std::vector<std::pair<ConnectionPtr, EventLoop*>> targets;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    targets.reserve(connections_.size());
    for (const auto& [id, connection] : connections_) {
      auto loop_it = connection_loops_.find(id);
      if (loop_it != connection_loops_.end()) {
        targets.emplace_back(connection, loop_it->second);
      }
    }
  }
  for (auto& [connection, loop] : targets) {
    loop->post([connection, payload = data] {
      if (!connection->closed()) {
        connection->send(payload);
      }
    });
  }
}

void TcpServer::inspectConnections(const std::function<void(const ConnectionPtr&)>& fn) const {
  std::vector<ConnectionPtr> snapshot;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    snapshot.reserve(connections_.size());
    for (const auto& [id, connection] : connections_) {
      snapshot.push_back(connection);
    }
  }
  for (const ConnectionPtr& connection : snapshot) {
    fn(connection);
  }
}

std::size_t TcpServer::connectionCount() const {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  return connections_.size();
}

void TcpServer::runForever() { acceptor_loop_.run(); }

void TcpServer::stop() {
  if (!started_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  acceptor_loop_.stop();
  if (listener_token_ != 0) {
    (void)acceptor_loop_.unregisterFd(listener_token_);
    listener_token_ = 0;
  }
  listener_.close();

  // Close connections on their own loops before tearing the loops down.
  std::vector<std::pair<ConnectionPtr, EventLoop*>> targets;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (const auto& [id, connection] : connections_) {
      auto loop_it = connection_loops_.find(id);
      if (loop_it != connection_loops_.end()) {
        targets.emplace_back(connection, loop_it->second);
      }
    }
  }
  for (auto& [connection, loop] : targets) {
    loop->post([connection] { connection->closeNow(); });
  }

  for (auto& io_thread : io_threads_) {
    io_thread->stop();
  }
  io_threads_.clear();

  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.clear();
    connection_loops_.clear();
  }
  BOURSE_LOG_INFO(options_.name, " stopped");
}

}  // namespace bourse::net
