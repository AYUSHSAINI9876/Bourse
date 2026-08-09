#include "bourse/net/connection.hpp"

#include <utility>

#include "bourse/core/clock.hpp"
#include "bourse/core/logger.hpp"
#include "bourse/core/metrics.hpp"

namespace bourse::net {
namespace {
/// Bounded per-read syscall size. Large enough that a pipelined burst arrives
/// in one or two reads, small enough that a single connection cannot make the
/// loop allocate megabytes before yielding to its peers.
constexpr std::size_t kReadChunk = 64 * 1024;

/// Above this, a peer is not draining and is costing the server memory. The
/// connection is dropped rather than allowed to grow without bound -- the
/// slow-consumer failure mode that takes down naive pub/sub servers.
constexpr std::size_t kMaxOutputBacklog = 16 * 1024 * 1024;
}  // namespace

Connection::Connection(EventLoop& loop, Socket socket, std::string peer, std::uint64_t id, CodecPtr codec)
    : loop_(loop),
      socket_(std::move(socket)),
      peer_(std::move(peer)),
      id_(id),
      codec_(std::move(codec)),
      input_(kReadChunk),
      output_(4096),
      created_at_ms_(nowMillis()) {}

Connection::~Connection() {
  if (registered_) {
    // Should not happen -- closeNow() unregisters -- but leaking a poller
    // registration would be far worse than an extra call here.
    (void)loop_.unregisterFd(token_);
  }
}

Status Connection::start() {
  BOURSE_TRY(socket_.setTcpNoDelay(true));

  ConnectionPtr guard = shared_from_this();
  Result<std::uint64_t> token = loop_.registerFd(static_cast<int>(socket_.handle()), kReadable,
                                                 [this, guard](std::uint32_t flags) { handleEvent(flags); });
  if (!token.ok()) {
    return token.status();
  }
  token_ = token.value();
  registered_ = true;

  MetricsRegistry::instance().counter("bourse_connections_accepted_total").increment();
  MetricsRegistry::instance().gauge("bourse_connections_active").add(1);

  codec_->onConnect(*this);
  return Status::success();
}

void Connection::handleEvent(std::uint32_t flags) {
  if (closed_) {
    return;
  }
  // Keep the object alive for the whole dispatch: a codec may close the
  // connection, which drops the server's reference.
  ConnectionPtr guard = shared_from_this();

  if ((flags & (kError | kHangup)) != 0 && (flags & kReadable) == 0) {
    closeNow();
    return;
  }
  if ((flags & kReadable) != 0) {
    handleRead();
  }
  if (!closed_ && (flags & kWritable) != 0) {
    handleWrite();
  }
}

void Connection::handleRead() {
  for (;;) {
    input_.ensureWritable(kReadChunk);
    const IoResult result = socket_.read(input_.beginWrite(), kReadChunk);

    if (result.outcome == IoOutcome::kOk) {
      input_.hasWritten(result.bytes);
      MetricsRegistry::instance().counter("bourse_network_bytes_read_total").increment(result.bytes);

      if (!codec_->onData(*this, input_)) {
        shutdownAfterFlush();
        return;
      }
      // A short read means the socket buffer is drained; going round again
      // would just cost an EAGAIN syscall.
      if (result.bytes < kReadChunk) {
        break;
      }
      continue;
    }

    if (result.outcome == IoOutcome::kWouldBlock || result.outcome == IoOutcome::kInterrupted) {
      break;
    }
    if (result.outcome == IoOutcome::kClosed) {
      closeNow();
      return;
    }

    BOURSE_LOG_DEBUG("read error on ", peer_, ": ", Socket::describeError(result.error_code));
    closeNow();
    return;
  }

  updateInterest();
}

void Connection::send(std::string_view data) {
  send(data.data(), data.size());
}

void Connection::send(const void* data, std::size_t length) {
  if (closed_ || length == 0) {
    return;
  }

  const auto* cursor = static_cast<const char*>(data);
  std::size_t remaining = length;

  // Fast path: nothing already queued, so try to hand the bytes straight to the
  // kernel. Buffering first and waiting for EPOLLOUT would add a full loop
  // iteration of latency to every reply.
  if (output_.readable() == 0) {
    const IoResult result = socket_.write(cursor, remaining);
    if (result.outcome == IoOutcome::kOk) {
      MetricsRegistry::instance().counter("bourse_network_bytes_written_total").increment(result.bytes);
      cursor += result.bytes;
      remaining -= result.bytes;
      if (remaining == 0) {
        return;
      }
    } else if (result.outcome == IoOutcome::kClosed || result.outcome == IoOutcome::kError) {
      closeNow();
      return;
    }
  }

  if (output_.readable() + remaining > kMaxOutputBacklog) {
    BOURSE_LOG_WARN("dropping ", peer_, ": output backlog exceeded ", kMaxOutputBacklog, " bytes");
    MetricsRegistry::instance().counter("bourse_connections_dropped_slow_total").increment();
    closeNow();
    return;
  }

  output_.append(cursor, remaining);
  updateInterest();
}

void Connection::handleWrite() {
  while (output_.readable() > 0) {
    const IoResult result = socket_.write(output_.peek(), output_.readable());
    if (result.outcome == IoOutcome::kOk) {
      output_.retrieve(result.bytes);
      MetricsRegistry::instance().counter("bourse_network_bytes_written_total").increment(result.bytes);
      continue;
    }
    if (result.outcome == IoOutcome::kWouldBlock || result.outcome == IoOutcome::kInterrupted) {
      break;
    }
    closeNow();
    return;
  }

  if (output_.readable() == 0) {
    if (close_after_flush_) {
      closeNow();
      return;
    }
    // Reclaim a buffer that ballooned from one large reply so an idle
    // connection does not hold megabytes forever.
    if (output_.capacity() > 64 * 1024) {
      output_.shrink(4096);
    }
  }
  updateInterest();
}

void Connection::updateInterest() {
  if (closed_ || !registered_) {
    return;
  }
  const std::uint32_t interest = output_.readable() > 0 ? (kReadable | kWritable) : kReadable;
  const Status status = loop_.updateInterest(token_, interest);
  if (!status.ok()) {
    BOURSE_LOG_DEBUG("updateInterest failed for ", peer_, ": ", status.toString());
  }
}

void Connection::shutdownAfterFlush() {
  if (closed_) {
    return;
  }
  if (output_.readable() == 0) {
    closeNow();
    return;
  }
  close_after_flush_ = true;
  updateInterest();
}

void Connection::closeNow() {
  if (closed_) {
    return;
  }
  closed_ = true;

  codec_->onClose(*this);

  if (registered_) {
    (void)loop_.unregisterFd(token_);
    registered_ = false;
  }
  socket_.close();

  MetricsRegistry::instance().gauge("bourse_connections_active").subtract(1);

  if (close_callback_) {
    // Copy the callback before invoking: it typically erases this connection
    // from the server's table, which can be the last owning reference.
    CloseCallback callback = close_callback_;
    close_callback_ = nullptr;
    callback(shared_from_this());
  }
}

void Connection::switchCodec(CodecPtr codec) {
  if (!codec) {
    return;
  }
  codec_->onClose(*this);
  codec_ = std::move(codec);
  codec_->onConnect(*this);
}

}  // namespace bourse::net
