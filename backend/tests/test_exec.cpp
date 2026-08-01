#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bourse/cache/keyspace.hpp"
#include "bourse/core/clock.hpp"
#include "bourse/exec/command.hpp"
#include "bourse/exec/pubsub.hpp"

using namespace bourse;
using namespace bourse::exec;

namespace {

/// Drives the command layer with no sockets involved. That the whole command
/// set is testable without a network is a direct consequence of commands
/// returning `Reply` objects rather than writing bytes.
class ExecFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    registry_ = CommandRegistry::createDefault();
    context_.keyspace = &keyspace_;
    context_.pubsub = &pubsub_;
    context_.started_at_ms = nowMillis();
  }

  Reply run(const std::vector<std::string>& argv) {
    CommandContext ctx{context_, nullptr};
    return registry_->dispatch(ctx, argv);
  }

  cache::Keyspace keyspace_;
  PubSub pubsub_;
  ServerContext context_;
  std::unique_ptr<CommandRegistry> registry_;
};

}  // namespace

TEST_F(ExecFixture, RegistryInstallsTheFullVerbSet) {
  EXPECT_GE(registry_->size(), 30u);
  for (const char* verb : {"GET", "SET", "DEL", "INCR", "EXPIRE", "LPUSH", "HSET", "SADD", "PING", "INFO"}) {
    EXPECT_NE(registry_->find(verb), nullptr) << "missing verb " << verb;
  }
}

TEST_F(ExecFixture, LookupIsCaseInsensitive) {
  EXPECT_NE(registry_->find("get"), nullptr);
  EXPECT_NE(registry_->find("GET"), nullptr);
  EXPECT_NE(registry_->find("GeT"), nullptr);
  EXPECT_EQ(registry_->find("nope"), nullptr);
}

TEST_F(ExecFixture, UnknownCommandIsAnErrorNotACrash) {
  const Reply reply = run({"TOTALLYFAKE", "a"});
  ASSERT_TRUE(reply.isError());
  EXPECT_NE(reply.text().find("unknown command"), std::string::npos);
}

TEST_F(ExecFixture, ArityIsValidatedCentrally) {
  // Fixed arity.
  EXPECT_TRUE(run({"GET"}).isError());
  EXPECT_TRUE(run({"GET", "a", "b"}).isError());
  // Variadic (negative) arity: at least N.
  EXPECT_TRUE(run({"DEL"}).isError());
  EXPECT_FALSE(run({"DEL", "a"}).isError());
  EXPECT_FALSE(run({"DEL", "a", "b", "c"}).isError());
}

TEST_F(ExecFixture, StringRoundTrip) {
  EXPECT_EQ(run({"SET", "k", "v"}).toResp(), "+OK\r\n");
  EXPECT_EQ(run({"GET", "k"}).toResp(), "$1\r\nv\r\n");
  EXPECT_EQ(run({"GET", "absent"}).toResp(), "$-1\r\n");
  EXPECT_EQ(run({"EXISTS", "k"}).integerValue(), 1);
  EXPECT_EQ(run({"DEL", "k"}).integerValue(), 1);
  EXPECT_EQ(run({"EXISTS", "k"}).integerValue(), 0);
}

TEST_F(ExecFixture, SetHonoursNxXxAndExpiry) {
  EXPECT_EQ(run({"SET", "k", "first", "NX"}).kind(), Reply::Kind::kSimpleString);
  EXPECT_EQ(run({"SET", "k", "second", "NX"}).kind(), Reply::Kind::kNull);
  EXPECT_EQ(run({"GET", "k"}).text(), "first");

  EXPECT_EQ(run({"SET", "k", "third", "XX"}).kind(), Reply::Kind::kSimpleString);
  EXPECT_EQ(run({"GET", "k"}).text(), "third");
  EXPECT_EQ(run({"SET", "brand-new", "v", "XX"}).kind(), Reply::Kind::kNull);

  EXPECT_FALSE(run({"SET", "k", "v", "EX", "100"}).isError());
  EXPECT_GT(run({"TTL", "k"}).integerValue(), 0);

  EXPECT_TRUE(run({"SET", "k", "v", "NX", "XX"}).isError());
  EXPECT_TRUE(run({"SET", "k", "v", "EX", "notanumber"}).isError());
  EXPECT_TRUE(run({"SET", "k", "v", "EX", "0"}).isError());
  EXPECT_TRUE(run({"SET", "k", "v", "BOGUS"}).isError());
}

TEST_F(ExecFixture, CounterVerbs) {
  EXPECT_EQ(run({"INCR", "c"}).integerValue(), 1);
  EXPECT_EQ(run({"INCRBY", "c", "9"}).integerValue(), 10);
  EXPECT_EQ(run({"DECR", "c"}).integerValue(), 9);
  EXPECT_EQ(run({"DECRBY", "c", "4"}).integerValue(), 5);
  EXPECT_TRUE(run({"INCRBY", "c", "notanumber"}).isError());

  run({"SET", "text", "abc"});
  EXPECT_TRUE(run({"INCR", "text"}).isError());
}

TEST_F(ExecFixture, WrongTypeErrorsUseTheRedisWording) {
  run({"LPUSH", "list", "a"});
  const Reply reply = run({"GET", "list"});
  ASSERT_TRUE(reply.isError());
  EXPECT_EQ(reply.text(), "WRONGTYPE Operation against a key holding the wrong kind of value");
}

TEST_F(ExecFixture, ListVerbs) {
  EXPECT_EQ(run({"RPUSH", "q", "a", "b", "c"}).integerValue(), 3);
  EXPECT_EQ(run({"LPUSH", "q", "z"}).integerValue(), 4);
  EXPECT_EQ(run({"LLEN", "q"}).integerValue(), 4);

  const Reply range = run({"LRANGE", "q", "0", "-1"});
  ASSERT_EQ(range.kind(), Reply::Kind::kArray);
  ASSERT_EQ(range.elements().size(), 4u);
  EXPECT_EQ(range.elements()[0].text(), "z");
  EXPECT_EQ(range.elements()[3].text(), "c");

  EXPECT_EQ(run({"LPOP", "q"}).text(), "z");
  EXPECT_EQ(run({"RPOP", "q"}).text(), "c");
  EXPECT_EQ(run({"LPOP", "empty-key"}).kind(), Reply::Kind::kNull);
  EXPECT_TRUE(run({"LRANGE", "q", "notanumber", "1"}).isError());
}

TEST_F(ExecFixture, HashVerbs) {
  EXPECT_EQ(run({"HSET", "h", "a", "1", "b", "2"}).integerValue(), 2);
  EXPECT_EQ(run({"HGET", "h", "a"}).text(), "1");
  EXPECT_EQ(run({"HGET", "h", "missing"}).kind(), Reply::Kind::kNull);
  EXPECT_EQ(run({"HLEN", "h"}).integerValue(), 2);

  const Reply all = run({"HGETALL", "h"});
  ASSERT_EQ(all.kind(), Reply::Kind::kArray);
  EXPECT_EQ(all.elements().size(), 4u) << "HGETALL must return a flat field/value array";

  EXPECT_EQ(run({"HDEL", "h", "a"}).integerValue(), 1);
  // Odd number of field/value arguments is a client error.
  EXPECT_TRUE(run({"HSET", "h", "onlyfield"}).isError());
}

TEST_F(ExecFixture, SetVerbs) {
  EXPECT_EQ(run({"SADD", "s", "a", "b", "a"}).integerValue(), 2);
  EXPECT_EQ(run({"SCARD", "s"}).integerValue(), 2);
  EXPECT_EQ(run({"SISMEMBER", "s", "a"}).integerValue(), 1);
  EXPECT_EQ(run({"SISMEMBER", "s", "zzz"}).integerValue(), 0);
  EXPECT_EQ(run({"SMEMBERS", "s"}).elements().size(), 2u);
  EXPECT_EQ(run({"SREM", "s", "a"}).integerValue(), 1);
}

TEST_F(ExecFixture, TtlVerbs) {
  run({"SET", "k", "v"});
  EXPECT_EQ(run({"TTL", "k"}).integerValue(), -1);
  EXPECT_EQ(run({"TTL", "absent"}).integerValue(), -2);
  EXPECT_EQ(run({"EXPIRE", "k", "100"}).integerValue(), 1);
  EXPECT_EQ(run({"EXPIRE", "absent", "100"}).integerValue(), 0);
  EXPECT_GT(run({"TTL", "k"}).integerValue(), 90);
  EXPECT_GT(run({"PTTL", "k"}).integerValue(), 90000);
  EXPECT_EQ(run({"PERSIST", "k"}).integerValue(), 1);
  EXPECT_EQ(run({"TTL", "k"}).integerValue(), -1);
}

TEST_F(ExecFixture, IntrospectionVerbs) {
  EXPECT_EQ(run({"PING"}).text(), "PONG");
  EXPECT_EQ(run({"PING", "hello"}).text(), "hello");
  EXPECT_EQ(run({"ECHO", "hi"}).text(), "hi");

  run({"SET", "a", "1"});
  run({"SET", "b", "2"});
  EXPECT_EQ(run({"DBSIZE"}).integerValue(), 2);

  const std::string info = run({"INFO"}).text();
  EXPECT_NE(info.find("bourse_version"), std::string::npos);
  EXPECT_NE(info.find("keyspace_hits"), std::string::npos);
  EXPECT_NE(info.find("maxmemory_policy"), std::string::npos);

  EXPECT_FALSE(run({"FLUSHALL"}).isError());
  EXPECT_EQ(run({"DBSIZE"}).integerValue(), 0);
}

TEST_F(ExecFixture, KeysUsesGlobMatching) {
  run({"SET", "user:1", "a"});
  run({"SET", "user:2", "b"});
  run({"SET", "order:1", "c"});
  EXPECT_EQ(run({"KEYS", "user:*"}).elements().size(), 2u);
  EXPECT_EQ(run({"KEYS", "*"}).elements().size(), 3u);
}

TEST_F(ExecFixture, SubscribeRequiresAConnection) {
  // With a null connection (the REST path) SUBSCRIBE must refuse cleanly
  // rather than dereferencing a null pointer.
  const Reply reply = run({"SUBSCRIBE", "channel"});
  EXPECT_TRUE(reply.isError());
}

// ---------------------------------------------------------------------------
// PubSub
// ---------------------------------------------------------------------------

TEST(PubSubTest, DeliversToSubscribersOnly) {
  PubSub pubsub;
  std::vector<std::pair<std::uint64_t, std::string>> delivered;
  pubsub.setDelivery([&](std::uint64_t id, const std::string&, const std::string& payload) {
    delivered.emplace_back(id, payload);
  });

  pubsub.subscribe(1, "news");
  pubsub.subscribe(2, "news");
  pubsub.subscribe(3, "sports");

  EXPECT_EQ(pubsub.publish("news", "hello"), 2u);
  EXPECT_EQ(delivered.size(), 2u);
  EXPECT_EQ(pubsub.publish("nobody-listening", "x"), 0u);
}

TEST(PubSubTest, UnsubscribeCleansUpEmptyChannels) {
  PubSub pubsub;
  pubsub.subscribe(1, "a");
  pubsub.subscribe(1, "b");
  EXPECT_EQ(pubsub.channelCount(), 2u);
  EXPECT_EQ(pubsub.subscriptionCount(1), 2u);

  EXPECT_EQ(pubsub.unsubscribe(1, "a"), 1u);
  EXPECT_EQ(pubsub.channelCount(), 1u) << "an empty channel must not linger";

  const std::vector<std::string> left = pubsub.unsubscribeAll(1);
  EXPECT_EQ(left, std::vector<std::string>{"b"});
  EXPECT_EQ(pubsub.channelCount(), 0u);
  EXPECT_EQ(pubsub.subscriptionCount(1), 0u);
}

TEST(PubSubTest, RemoveSubscriberIsIdempotent) {
  PubSub pubsub;
  pubsub.subscribe(7, "c");
  pubsub.removeSubscriber(7);
  pubsub.removeSubscriber(7);  // must not throw or corrupt state
  EXPECT_EQ(pubsub.channelCount(), 0u);
  EXPECT_EQ(pubsub.publish("c", "x"), 0u);
}
