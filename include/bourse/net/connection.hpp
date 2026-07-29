#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "bourse/core/byte_buffer.hpp"
#include "bourse/core/socket.hpp"
#include "bourse/net/event_loop.hpp"

namespace bourse::net {

class Connection;
using ConnectionPtr = std::shared_ptr<Connection>;

/// Strategy interface for wire protocols.
///
/// A codec owns exactly one job: turn bytes into application events and back.
/// It never touches the socket, never registers with the poller, and never
/// decides when to close. That separation is why RESP, HTTP and WebSocket can
/// share one connection implementation, and why a WebSocket upgrade is just a
/// codec swap on a live connection rather than a special case in the server.
class ProtocolCodec {
 public:
  virtual ~ProtocolCodec() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// Consumes as many complete frames as `input` holds, writing replies via
  /// `connection.send()`. Returning false asks the server to close the
  /// connection -- used for protocol errors and for `QUIT`.
  ///
  /// Partial frames must be left in `input` untouched; the next readable event
  /// will deliver the rest. Getting this wrong is the classic TCP bug, so the
  /// codec tests deliberately feed data one byte at a time.
  virtual bool onData(Connection& connection, ByteBuffer& input) = 0;

  virtual void onConnect(Connection& /*connection*/) {}
  virtual void onClose(Connection& /*connection*/) {}
};

using CodecPtr = std::unique_ptr<ProtocolCodec>;

/// One client connection, owned by exactly one EventLoop.
///
/// Lifetime is managed with shared_ptr because a close can be initiated from
/// three places -- the peer, a codec, or server shutdown -- and the object must
/// outlive whichever handler is mid-flight. Handlers take a
/// `shared_from_this()` guard for exactly that reason.
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  using CloseCallback = std::function<void(const ConnectionPtr&)>;

  Connection(EventLoop& loop, Socket socket, std::string peer, std::uint64_t id, CodecPtr codec);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  /// Registers with the loop and fires the codec's onConnect. Must run on the
  /// owning loop thread.
  Status start();

  /// Queues bytes for delivery.
  ///
  /// Writes are attempted inline first: for the common case of a small reply on
  /// an idle socket this completes in one syscall with no allocation and no
  /// poller round-trip. Only a partial write parks the remainder in the output
  /// buffer and arms EPOLLOUT, which is what stops a slow consumer from
  /// blocking the loop.
  void send(std::string_view data);
  void send(const void* data, std::size_t length);

  /// Closes once the output buffer has drained.
  void shutdownAfterFlush();
  /// Closes immediately, discarding buffered output.
  void closeNow();

  /// Replaces the codec on a live connection (the WebSocket upgrade path).
  void switchCodec(CodecPtr codec);

  [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
  [[nodiscard]] const std::string& peer() const noexcept { return peer_; }
  [[nodiscard]] bool closed() const noexcept { return closed_; }
  [[nodiscard]] EventLoop& loop() noexcept { return loop_; }
  [[nodiscard]] std::size_t pendingOutputBytes() const noexcept { return output_.readable(); }
  [[nodiscard]] std::string_view codecName() const noexcept { return codec_->name(); }

  /// Free-form per-connection state used by codecs (the RESP codec keeps its
  /// subscription set here, HTTP keeps keep-alive state).
  void setUserData(std::shared_ptr<void> data) { user_data_ = std::move(data); }
  [[nodiscard]] const std::shared_ptr<void>& userData() const noexcept { return user_data_; }

  void setCloseCallback(CloseCallback callback) { close_callback_ = std::move(callback); }

  [[nodiscard]] std::int64_t createdAtMillis() const noexcept { return created_at_ms_; }

 private:
  void handleEvent(std::uint32_t flags);
  void handleRead();
  void handleWrite();
  void updateInterest();

  EventLoop& loop_;
  Socket socket_;
  std::string peer_;
  std::uint64_t id_;
  CodecPtr codec_;

  ByteBuffer input_;
  ByteBuffer output_;

  std::uint64_t token_ = 0;
  bool registered_ = false;
  bool closed_ = false;
  bool close_after_flush_ = false;
  std::int64_t created_at_ms_ = 0;

  std::shared_ptr<void> user_data_;
  CloseCallback close_callback_;
};

}  // namespace bourse::net
