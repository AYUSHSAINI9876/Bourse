#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bourse/cache/keyspace.hpp"
#include "bourse/core/clock.hpp"
#include "bourse/core/socket.hpp"
#include "bourse/exec/command.hpp"
#include "bourse/exec/pubsub.hpp"
#include "bourse/net/event_loop.hpp"
#include "bourse/net/poller.hpp"
#include "bourse/net/resp_codec.hpp"
#include "bourse/net/server.hpp"

using namespace bourse;
using namespace bourse::net;

// ---------------------------------------------------------------------------
// Poller
// ---------------------------------------------------------------------------

TEST(PollerTest, FactoryPicksTheBestBackend) {
  auto poller = Poller::create();
  ASSERT_NE(poller, nullptr);
#if defined(BOURSE_HAVE_EPOLL)
  EXPECT_EQ(poller->name(), "epoll");
#endif
  EXPECT_EQ(Poller::createPortable()->name(), "poll");
}

TEST(PollerTest, RejectsUnknownDescriptors) {
  auto poller = Poller::createPortable();
  EXPECT_FALSE(poller->modify(4242, kReadable, 1).ok());
  EXPECT_FALSE(poller->remove(4242).ok());
}

// ---------------------------------------------------------------------------
// EventLoop
// ---------------------------------------------------------------------------

TEST(EventLoopTest, RunsPostedTasksOnTheLoopThread) {
  EventLoop loop;
  std::atomic<int> counter{0};
  std::thread::id task_thread;

  std::thread runner([&] { loop.run(); });
  // Capture the id while the thread is still running: after join() a
  // std::thread compares equal to a default-constructed id, not to itself.
  const std::thread::id runner_id = runner.get_id();

  // Posting from a foreign thread must wake the loop out of its blocking wait.
  for (int i = 0; i < 100; ++i) {
    loop.post([&] {
      task_thread = std::this_thread::get_id();
      counter.fetch_add(1, std::memory_order_relaxed);
    });
  }

  const std::int64_t deadline = nowMillis() + 5000;
  while (counter.load(std::memory_order_relaxed) < 100 && nowMillis() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  loop.stop();
  runner.join();

  EXPECT_EQ(counter.load(), 100);
  EXPECT_EQ(task_thread, runner_id) << "posted tasks must run on the loop thread, not the caller's";
}

TEST(EventLoopTest, OneShotTimerFiresOnce) {
  EventLoop loop;
  std::atomic<int> fired{0};
  loop.scheduleAfter(10, [&] { fired.fetch_add(1, std::memory_order_relaxed); });

  const std::int64_t deadline = nowMillis() + 2000;
  while (fired.load() == 0 && nowMillis() < deadline) {
    loop.runOnce(5);
  }
  for (int i = 0; i < 20; ++i) {
    loop.runOnce(5);
  }
  EXPECT_EQ(fired.load(), 1);
}

TEST(EventLoopTest, RepeatingTimerKeepsFiring) {
  EventLoop loop;
  std::atomic<int> ticks{0};
  loop.scheduleEvery(5, [&] { ticks.fetch_add(1, std::memory_order_relaxed); });

  const std::int64_t deadline = nowMillis() + 3000;
  while (ticks.load() < 3 && nowMillis() < deadline) {
    loop.runOnce(5);
  }
  EXPECT_GE(ticks.load(), 3);
}

TEST(EventLoopTest, CancelledTimerNeverFires) {
  EventLoop loop;
  std::atomic<int> fired{0};
  const std::uint64_t id = loop.scheduleAfter(20, [&] { fired.fetch_add(1, std::memory_order_relaxed); });
  loop.cancelTimer(id);

  const std::int64_t deadline = nowMillis() + 200;
  while (nowMillis() < deadline) {
    loop.runOnce(5);
  }
  EXPECT_EQ(fired.load(), 0);
}

TEST(EventLoopThreadTest, LoopIsUsableImmediatelyAfterConstruction) {
  // The constructor blocks until the loop exists, so this post cannot race.
  EventLoopThread thread("test");
  std::atomic<bool> ran{false};
  thread.loop().post([&] { ran.store(true, std::memory_order_release); });

  const std::int64_t deadline = nowMillis() + 3000;
  while (!ran.load(std::memory_order_acquire) && nowMillis() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(ran.load());
  thread.stop();
}

// ---------------------------------------------------------------------------
// Full server, driven over a real socket
// ---------------------------------------------------------------------------

namespace {

class ServerFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Socket::initializeNetworking().ok());
    registry_ = exec::CommandRegistry::createDefault();
    context_.keyspace = &keyspace_;
    context_.pubsub = &pubsub_;
    context_.started_at_ms = nowMillis();

    ServerOptions options;
    options.host = "127.0.0.1";
    options.port = 0;  // ephemeral -- lets tests run in parallel
    options.io_threads = 2;
    options.name = "test";

    server_ = std::make_unique<TcpServer>(
        options, [this] { return std::make_unique<RespCodec>(*registry_, context_); });
    ASSERT_TRUE(server_->start().ok());
    port_ = server_->port();
    ASSERT_NE(port_, 0);

    acceptor_ = std::thread([this] { server_->runForever(); });
  }

  void TearDown() override {
    // Ask the acceptor loop to exit and join it *before* tearing the server
    // down, so shutdown is single-threaded.
    server_->acceptorLoop().stop();
    if (acceptor_.joinable()) {
      acceptor_.join();
    }
    server_->stop();
  }

  Socket connect() {
    Result<Socket> client = Socket::connectTcp("127.0.0.1", port_);
    EXPECT_TRUE(client.ok()) << client.status().toString();
    return std::move(client).value();
  }

  static void writeAll(const Socket& socket, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const IoResult result = socket.write(data.data() + sent, data.size() - sent);
      if (result.retryable()) {
        continue;
      }
      ASSERT_TRUE(result.ok()) << "write failed";
      sent += result.bytes;
    }
  }

  /// Reads until `expected_bytes` have arrived or the deadline passes.
  static std::string readReply(const Socket& socket, std::size_t expected_bytes, int timeout_ms = 3000) {
    std::string out;
    char buffer[4096];
    const std::int64_t deadline = nowMillis() + timeout_ms;
    while (out.size() < expected_bytes && nowMillis() < deadline) {
      const IoResult result = socket.read(buffer, sizeof(buffer));
      if (result.ok() && result.bytes > 0) {
        out.append(buffer, result.bytes);
        continue;
      }
      if (result.retryable()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      break;
    }
    return out;
  }

  cache::Keyspace keyspace_;
  exec::PubSub pubsub_;
  exec::ServerContext context_;
  std::unique_ptr<exec::CommandRegistry> registry_;
  std::unique_ptr<TcpServer> server_;
  std::thread acceptor_;
  std::uint16_t port_ = 0;
};

}  // namespace

TEST_F(ServerFixture, AnswersPingOverTheWire) {
  Socket client = connect();
  writeAll(client, "*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(readReply(client, 7), "+PONG\r\n");
}

TEST_F(ServerFixture, RoundTripsAValue) {
  Socket client = connect();
  writeAll(client, "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
  EXPECT_EQ(readReply(client, 5), "+OK\r\n");

  writeAll(client, "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
  EXPECT_EQ(readReply(client, 9), "$3\r\nbar\r\n");
}

TEST_F(ServerFixture, HandlesRequestsSplitAcrossPackets) {
  // The single most important network test: a command delivered one byte at a
  // time, with real TCP in between, must produce exactly one reply.
  Socket client = connect();
  const std::string request = "*3\r\n$3\r\nSET\r\n$5\r\nsplit\r\n$2\r\nok\r\n";
  for (char c : request) {
    writeAll(client, std::string_view(&c, 1));
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  EXPECT_EQ(readReply(client, 5), "+OK\r\n");

  auto stored = keyspace_.get("split");
  ASSERT_TRUE(stored.ok());
  ASSERT_TRUE(stored.value().has_value());
  EXPECT_EQ(*stored.value(), "ok");
}

TEST_F(ServerFixture, PipelinesManyCommandsInOneWrite) {
  Socket client = connect();
  std::string batch;
  for (int i = 0; i < 50; ++i) {
    batch += "*1\r\n$4\r\nPING\r\n";
  }
  writeAll(client, batch);

  const std::string replies = readReply(client, 50 * 7);
  EXPECT_EQ(replies.size(), 50u * 7);
  for (int i = 0; i < 50; ++i) {
    EXPECT_EQ(replies.substr(static_cast<std::size_t>(i) * 7, 7), "+PONG\r\n");
  }
}

TEST_F(ServerFixture, AcceptsInlineCommands) {
  Socket client = connect();
  writeAll(client, "PING\r\n");
  EXPECT_EQ(readReply(client, 7), "+PONG\r\n");
}

TEST_F(ServerFixture, ServesManyConcurrentConnections) {
  constexpr int kClients = 32;
  std::vector<std::thread> workers;
  std::atomic<int> succeeded{0};

  for (int i = 0; i < kClients; ++i) {
    workers.emplace_back([this, i, &succeeded] {
      Result<Socket> maybe = Socket::connectTcp("127.0.0.1", port_);
      if (!maybe.ok()) {
        return;
      }
      Socket client = std::move(maybe).value();
      const std::string key = "client:" + std::to_string(i);
      const std::string request =
          "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n$1\r\n1\r\n";
      writeAll(client, request);
      if (readReply(client, 5) == "+OK\r\n") {
        succeeded.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(succeeded.load(), kClients);
  EXPECT_EQ(keyspace_.size(), static_cast<std::size_t>(kClients));
}

TEST_F(ServerFixture, ClosesTheConnectionOnProtocolError) {
  Socket client = connect();
  writeAll(client, "*2\r\n+NOTABULK\r\n");
  const std::string reply = readReply(client, 1, 1000);
  EXPECT_FALSE(reply.empty());
  EXPECT_EQ(reply[0], '-') << "expected a protocol error reply";
}

TEST_F(ServerFixture, TracksConnectionCount) {
  EXPECT_EQ(server_->connectionCount(), 0u);
  {
    Socket client = connect();
    writeAll(client, "*1\r\n$4\r\nPING\r\n");
    EXPECT_EQ(readReply(client, 7), "+PONG\r\n");
    EXPECT_EQ(server_->connectionCount(), 1u);
  }
  // After the client goes away the server must reap the entry.
  const std::int64_t deadline = nowMillis() + 3000;
  while (server_->connectionCount() != 0 && nowMillis() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(server_->connectionCount(), 0u);
}

TEST_F(ServerFixture, PubSubDeliversAcrossConnections) {
  // Wire delivery the same way the real server does, then prove a message
  // published by one connection reaches a subscriber on another.
  pubsub_.setDelivery([this](std::uint64_t id, const std::string& channel, const std::string& payload) {
    exec::Reply message =
        exec::Reply::array({exec::Reply::bulkString("message"), exec::Reply::bulkString(channel),
                            exec::Reply::bulkString(payload)});
    server_->sendTo(id, message.toResp());
  });

  Socket subscriber = connect();
  writeAll(subscriber, "*2\r\n$9\r\nSUBSCRIBE\r\n$4\r\nnews\r\n");
  const std::string confirmation = readReply(subscriber, 30);
  EXPECT_NE(confirmation.find("subscribe"), std::string::npos);

  Socket publisher = connect();
  writeAll(publisher, "*3\r\n$7\r\nPUBLISH\r\n$4\r\nnews\r\n$5\r\nhello\r\n");
  EXPECT_EQ(readReply(publisher, 4), ":1\r\n");

  const std::string pushed = readReply(subscriber, 30);
  EXPECT_NE(pushed.find("hello"), std::string::npos);
  EXPECT_NE(pushed.find("news"), std::string::npos);
}
