#include <algorithm>
#include <cctype>
#include <sstream>

#include "bourse/cache/keyspace.hpp"
#include "bourse/core/clock.hpp"
#include "bourse/core/metrics.hpp"
#include "bourse/exec/command.hpp"
#include "bourse/exec/pubsub.hpp"
#include "bourse/net/connection.hpp"

namespace bourse::exec {
namespace {

using cache::Keyspace;
using cache::Value;
using cache::ValueType;

/// Converts an internal Status into the RESP error a client should see.
/// Centralised so the WRONGTYPE wording is identical everywhere.
Reply errorFrom(const Status& status) {
  switch (status.code()) {
    case ErrorCode::kWrongType: return Reply::error(status.message());
    case ErrorCode::kOutOfMemory: return Reply::error("OOM " + status.message());
    default: return Reply::error("ERR " + status.message());
  }
}

std::string toUpper(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

Reply parseIntegerArgument(std::string_view text, std::int64_t& out) {
  Result<std::int64_t> parsed = cache::parseInteger(text);
  if (!parsed.ok()) {
    return Reply::error("ERR value is not an integer or out of range");
  }
  out = parsed.value();
  return Reply::ok();
}

void add(CommandRegistry& registry, std::string name, int arity, bool is_write, std::string summary,
         LambdaCommand::Handler handler, bool is_admin = false, bool is_no_auth = false) {
  registry.registerCommand(std::make_unique<LambdaCommand>(
      std::move(name), arity, is_write, std::move(summary), std::move(handler), is_admin, is_no_auth));
}

// ---------------------------------------------------------------------------
// SET -- an explicit class because of its option grammar
// ---------------------------------------------------------------------------

/// `SET key value [EX seconds | PX milliseconds] [NX | XX]`
///
/// Written as a real class rather than a lambda: option parsing plus the
/// conditional-set semantics is enough logic that it deserves a name, a home
/// for its constants, and its own unit test.
class SetCommand final : public Command {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "SET"; }

  [[nodiscard]] int arity() const noexcept override { return -3; }

  [[nodiscard]] bool isWrite() const noexcept override { return true; }

  [[nodiscard]] std::string_view summary() const noexcept override {
    return "SET key value [EX seconds|PX milliseconds] [NX|XX]";
  }

  Reply execute(CommandContext& context, const std::vector<std::string>& argv) override {
    Keyspace& keyspace = *context.server.keyspace;

    std::int64_t ttl_ms = 0;
    bool only_if_absent = false;
    bool only_if_present = false;

    for (std::size_t i = 3; i < argv.size(); ++i) {
      const std::string option = toUpper(argv[i]);
      if (option == "NX") {
        only_if_absent = true;
      } else if (option == "XX") {
        only_if_present = true;
      } else if (option == "EX" || option == "PX") {
        if (i + 1 >= argv.size()) {
          return Reply::error("ERR syntax error");
        }
        std::int64_t amount = 0;
        const Reply parse_error = parseIntegerArgument(argv[i + 1], amount);
        if (parse_error.isError()) {
          return parse_error;
        }
        if (amount <= 0) {
          return Reply::error("ERR invalid expire time in 'set' command");
        }
        ttl_ms = option == "EX" ? amount * 1000 : amount;
        ++i;
      } else {
        return Reply::error("ERR syntax error");
      }
    }

    if (only_if_absent && only_if_present) {
      return Reply::error("ERR syntax error");
    }

    const bool exists = keyspace.exists(argv[1]);
    if ((only_if_absent && exists) || (only_if_present && !exists)) {
      return Reply::null();
    }

    const Status status = keyspace.set(argv[1], Value::makeString(argv[2]), ttl_ms);
    if (!status.ok()) {
      return errorFrom(status);
    }
    return Reply::ok();
  }
};

// ---------------------------------------------------------------------------
// SUBSCRIBE / UNSUBSCRIBE -- explicit classes because they need the connection
// ---------------------------------------------------------------------------

class SubscribeCommand final : public Command {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "SUBSCRIBE"; }

  [[nodiscard]] int arity() const noexcept override { return -2; }

  [[nodiscard]] std::string_view summary() const noexcept override {
    return "SUBSCRIBE channel [channel ...]";
  }

  Reply execute(CommandContext& context, const std::vector<std::string>& argv) override {
    if (context.connection == nullptr) {
      return Reply::error("ERR SUBSCRIBE is only available on a RESP connection");
    }
    if (context.server.pubsub == nullptr) {
      return Reply::error("ERR pub/sub is not available");
    }

    // Redis emits one confirmation array per channel. Everything after the
    // first is pushed directly, because a single command may only return one
    // Reply -- so the extras go straight down the socket.
    std::vector<Reply> first;
    std::string extra;

    for (std::size_t i = 1; i < argv.size(); ++i) {
      const std::size_t count = context.server.pubsub->subscribe(context.connection->id(), argv[i]);
      Reply confirmation = Reply::array({Reply::bulkString("subscribe"), Reply::bulkString(argv[i]),
                                         Reply::integer(static_cast<std::int64_t>(count))});
      if (i == 1) {
        first = confirmation.elements();
      } else {
        confirmation.encodeResp(extra);
      }
    }
    if (!extra.empty()) {
      context.connection->send(extra);
    }
    return Reply::array(std::move(first));
  }
};

class UnsubscribeCommand final : public Command {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "UNSUBSCRIBE"; }

  [[nodiscard]] int arity() const noexcept override { return -1; }

  [[nodiscard]] std::string_view summary() const noexcept override { return "UNSUBSCRIBE [channel ...]"; }

  Reply execute(CommandContext& context, const std::vector<std::string>& argv) override {
    if (context.connection == nullptr || context.server.pubsub == nullptr) {
      return Reply::error("ERR UNSUBSCRIBE is only available on a RESP connection");
    }
    const std::uint64_t id = context.connection->id();

    if (argv.size() == 1) {
      const std::vector<std::string> channels = context.server.pubsub->unsubscribeAll(id);
      if (channels.empty()) {
        return Reply::array({Reply::bulkString("unsubscribe"), Reply::null(), Reply::integer(0)});
      }
      std::string extra;
      Reply first;
      for (std::size_t i = 0; i < channels.size(); ++i) {
        Reply confirmation =
            Reply::array({Reply::bulkString("unsubscribe"), Reply::bulkString(channels[i]),
                          Reply::integer(static_cast<std::int64_t>(channels.size() - i - 1))});
        if (i == 0) {
          first = std::move(confirmation);
        } else {
          confirmation.encodeResp(extra);
        }
      }
      if (!extra.empty()) {
        context.connection->send(extra);
      }
      return first;
    }

    std::string extra;
    Reply first;
    for (std::size_t i = 1; i < argv.size(); ++i) {
      const std::size_t remaining = context.server.pubsub->unsubscribe(id, argv[i]);
      Reply confirmation = Reply::array({Reply::bulkString("unsubscribe"), Reply::bulkString(argv[i]),
                                         Reply::integer(static_cast<std::int64_t>(remaining))});
      if (i == 1) {
        first = std::move(confirmation);
      } else {
        confirmation.encodeResp(extra);
      }
    }
    if (!extra.empty()) {
      context.connection->send(extra);
    }
    return first;
  }
};

// ---------------------------------------------------------------------------
// INFO -- explicit class; it aggregates every subsystem's stats
// ---------------------------------------------------------------------------

class InfoCommand final : public Command {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "INFO"; }

  [[nodiscard]] int arity() const noexcept override { return -1; }

  [[nodiscard]] std::string_view summary() const noexcept override { return "INFO [section]"; }

  Reply execute(CommandContext& context, const std::vector<std::string>& /*argv*/) override {
    const cache::KeyspaceStats stats = context.server.keyspace->stats();
    const std::int64_t uptime_ms = nowMillis() - context.server.started_at_ms;

    std::ostringstream out;
    out << "# Server\r\n";
    out << "bourse_version:" << context.server.version << "\r\n";
    out << "uptime_in_seconds:" << (uptime_ms / 1000) << "\r\n";
    out << "\r\n# Keyspace\r\n";
    out << "keys:" << stats.keys << "\r\n";
    out << "used_memory:" << stats.memory_bytes << "\r\n";
    out << "keyspace_hits:" << stats.hits << "\r\n";
    out << "keyspace_misses:" << stats.misses << "\r\n";
    out << "expired_keys:" << stats.expired << "\r\n";
    out << "evicted_keys:" << stats.evicted << "\r\n";
    out << "maxmemory_policy:" << context.server.keyspace->evictionPolicyName() << "\r\n";
    out << "\r\n# Stats\r\n";
    out << "total_commands_processed:"
        << MetricsRegistry::instance().counter("bourse_commands_processed_total").value() << "\r\n";
    out << "total_connections_received:"
        << MetricsRegistry::instance().counter("bourse_connections_accepted_total").value() << "\r\n";
    out << "connected_clients:" << MetricsRegistry::instance().gauge("bourse_connections_active").value()
        << "\r\n";

    const Histogram::Snapshot latency =
        MetricsRegistry::instance().histogram("bourse_command_latency_nanos").snapshot();
    out << "command_latency_p50_ns:" << latency.p50 << "\r\n";
    out << "command_latency_p99_ns:" << latency.p99 << "\r\n";

    if (context.server.pubsub != nullptr) {
      out << "\r\n# Pubsub\r\n";
      out << "pubsub_channels:" << context.server.pubsub->channelCount() << "\r\n";
    }
    return Reply::bulkString(out.str());
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

void registerKeyspaceCommands(CommandRegistry& registry) {
  registry.registerCommand(std::make_unique<SetCommand>());

  add(registry, "GET", 2, false, "GET key", [](CommandContext& ctx, const std::vector<std::string>& argv) {
    Result<std::optional<std::string>> value = ctx.server.keyspace->get(argv[1]);
    if (!value.ok()) {
      return errorFrom(value.status());
    }
    if (!value.value().has_value()) {
      return Reply::null();
    }
    return Reply::bulkString(*value.value());
  });

  add(registry, "SETEX", 4, true, "SETEX key seconds value",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        std::int64_t seconds = 0;
        const Reply parse_error = parseIntegerArgument(argv[2], seconds);
        if (parse_error.isError()) {
          return parse_error;
        }
        if (seconds <= 0) {
          return Reply::error("ERR invalid expire time in 'setex' command");
        }
        const Status status = ctx.server.keyspace->set(argv[1], Value::makeString(argv[3]), seconds * 1000);
        return status.ok() ? Reply::ok() : errorFrom(status);
      });

  add(registry, "DEL", -2, true, "DEL key [key ...]",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        const std::vector<std::string> keys(argv.begin() + 1, argv.end());
        return Reply::integer(static_cast<std::int64_t>(ctx.server.keyspace->remove(keys)));
      });

  add(registry, "EXISTS", -2, false, "EXISTS key [key ...]",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        std::int64_t found = 0;
        for (std::size_t i = 1; i < argv.size(); ++i) {
          if (ctx.server.keyspace->exists(argv[i])) {
            ++found;
          }
        }
        return Reply::integer(found);
      });

  add(registry, "INCR", 2, true, "INCR key", [](CommandContext& ctx, const std::vector<std::string>& argv) {
    Result<std::int64_t> value = ctx.server.keyspace->incrementBy(argv[1], 1);
    return value.ok() ? Reply::integer(value.value()) : errorFrom(value.status());
  });

  add(registry, "DECR", 2, true, "DECR key", [](CommandContext& ctx, const std::vector<std::string>& argv) {
    Result<std::int64_t> value = ctx.server.keyspace->incrementBy(argv[1], -1);
    return value.ok() ? Reply::integer(value.value()) : errorFrom(value.status());
  });

  add(registry, "INCRBY", 3, true, "INCRBY key increment",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        std::int64_t delta = 0;
        const Reply parse_error = parseIntegerArgument(argv[2], delta);
        if (parse_error.isError()) {
          return parse_error;
        }
        Result<std::int64_t> value = ctx.server.keyspace->incrementBy(argv[1], delta);
        return value.ok() ? Reply::integer(value.value()) : errorFrom(value.status());
      });

  add(registry, "DECRBY", 3, true, "DECRBY key decrement",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        std::int64_t delta = 0;
        const Reply parse_error = parseIntegerArgument(argv[2], delta);
        if (parse_error.isError()) {
          return parse_error;
        }
        Result<std::int64_t> value = ctx.server.keyspace->incrementBy(argv[1], -delta);
        return value.ok() ? Reply::integer(value.value()) : errorFrom(value.status());
      });

  add(registry, "APPEND", 3, true, "APPEND key value",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        Result<std::size_t> length = ctx.server.keyspace->appendString(argv[1], argv[2]);
        return length.ok() ? Reply::integer(static_cast<std::int64_t>(length.value()))
                           : errorFrom(length.status());
      });

  add(registry, "STRLEN", 2, false, "STRLEN key",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        Result<std::size_t> length = ctx.server.keyspace->stringLength(argv[1]);
        return length.ok() ? Reply::integer(static_cast<std::int64_t>(length.value()))
                           : errorFrom(length.status());
      });

  add(registry, "EXPIRE", 3, true, "EXPIRE key seconds",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        std::int64_t seconds = 0;
        const Reply parse_error = parseIntegerArgument(argv[2], seconds);
        if (parse_error.isError()) {
          return parse_error;
        }
        return Reply::integer(ctx.server.keyspace->expire(argv[1], seconds * 1000) ? 1 : 0);
      });

  add(registry, "PEXPIRE", 3, true, "PEXPIRE key milliseconds",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        std::int64_t millis = 0;
        const Reply parse_error = parseIntegerArgument(argv[2], millis);
        if (parse_error.isError()) {
          return parse_error;
        }
        return Reply::integer(ctx.server.keyspace->expire(argv[1], millis) ? 1 : 0);
      });

  add(registry, "TTL", 2, false, "TTL key", [](CommandContext& ctx, const std::vector<std::string>& argv) {
    const std::int64_t ttl = ctx.server.keyspace->ttlMillis(argv[1]);
    if (ttl < 0) {
      return Reply::integer(ttl);  // -1 no TTL, -2 no key
    }
    return Reply::integer((ttl + 999) / 1000);  // round up, as Redis does
  });

  add(registry, "PTTL", 2, false, "PTTL key", [](CommandContext& ctx, const std::vector<std::string>& argv) {
    return Reply::integer(ctx.server.keyspace->ttlMillis(argv[1]));
  });

  add(registry, "PERSIST", 2, true, "PERSIST key",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        return Reply::integer(ctx.server.keyspace->persist(argv[1]) ? 1 : 0);
      });

  add(registry, "TYPE", 2, false, "TYPE key", [](CommandContext& ctx, const std::vector<std::string>& argv) {
    Result<ValueType> type = ctx.server.keyspace->typeOf(argv[1]);
    return type.ok() ? Reply::simpleString(cache::toString(type.value())) : errorFrom(type.status());
  });

  add(registry, "KEYS", 2, false, "KEYS pattern",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        std::vector<std::string> keys = ctx.server.keyspace->matchingKeys(argv[1]);
        std::sort(keys.begin(), keys.end());
        return Reply::stringArray(keys);
      });
}

void registerListCommands(CommandRegistry& registry) {
  add(registry, "LPUSH", -3, true, "LPUSH key value [value ...]",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        const std::vector<std::string> values(argv.begin() + 2, argv.end());
        Result<std::size_t> length = ctx.server.keyspace->listPush(argv[1], values, true);
        return length.ok() ? Reply::integer(static_cast<std::int64_t>(length.value()))
                           : errorFrom(length.status());
      });

  add(registry, "RPUSH", -3, true, "RPUSH key value [value ...]",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        const std::vector<std::string> values(argv.begin() + 2, argv.end());
        Result<std::size_t> length = ctx.server.keyspace->listPush(argv[1], values, false);
        return length.ok() ? Reply::integer(static_cast<std::int64_t>(length.value()))
                           : errorFrom(length.status());
      });

  add(registry, "LPOP", 2, true, "LPOP key", [](CommandContext& ctx, const std::vector<std::string>& argv) {
    Result<std::optional<std::string>> popped = ctx.server.keyspace->listPop(argv[1], true);
    if (!popped.ok()) {
      return errorFrom(popped.status());
    }
    return popped.value().has_value() ? Reply::bulkString(*popped.value()) : Reply::null();
  });

  add(registry, "RPOP", 2, true, "RPOP key", [](CommandContext& ctx, const std::vector<std::string>& argv) {
    Result<std::optional<std::string>> popped = ctx.server.keyspace->listPop(argv[1], false);
    if (!popped.ok()) {
      return errorFrom(popped.status());
    }
    return popped.value().has_value() ? Reply::bulkString(*popped.value()) : Reply::null();
  });

  add(registry, "LLEN", 2, false, "LLEN key", [](CommandContext& ctx, const std::vector<std::string>& argv) {
    Result<std::size_t> length = ctx.server.keyspace->listLength(argv[1]);
    return length.ok() ? Reply::integer(static_cast<std::int64_t>(length.value()))
                       : errorFrom(length.status());
  });

  add(registry, "LRANGE", 4, false, "LRANGE key start stop",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        std::int64_t start = 0;
        std::int64_t stop = 0;
        Reply parse_error = parseIntegerArgument(argv[2], start);
        if (parse_error.isError()) {
          return parse_error;
        }
        parse_error = parseIntegerArgument(argv[3], stop);
        if (parse_error.isError()) {
          return parse_error;
        }
        Result<std::vector<std::string>> range = ctx.server.keyspace->listRange(argv[1], start, stop);
        return range.ok() ? Reply::stringArray(range.value()) : errorFrom(range.status());
      });
}

void registerCollectionCommands(CommandRegistry& registry) {
  add(registry, "HSET", -4, true, "HSET key field value [field value ...]",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        if ((argv.size() - 2) % 2 != 0) {
          return Reply::error("ERR wrong number of arguments for 'hset' command");
        }
        std::vector<std::pair<std::string, std::string>> fields;
        fields.reserve((argv.size() - 2) / 2);
        for (std::size_t i = 2; i + 1 < argv.size(); i += 2) {
          fields.emplace_back(argv[i], argv[i + 1]);
        }
        Result<std::size_t> added = ctx.server.keyspace->hashSet(argv[1], fields);
        return added.ok() ? Reply::integer(static_cast<std::int64_t>(added.value()))
                          : errorFrom(added.status());
      });

  add(registry, "HGET", 3, false, "HGET key field",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        Result<std::optional<std::string>> value = ctx.server.keyspace->hashGet(argv[1], argv[2]);
        if (!value.ok()) {
          return errorFrom(value.status());
        }
        return value.value().has_value() ? Reply::bulkString(*value.value()) : Reply::null();
      });

  add(registry, "HGETALL", 2, false, "HGETALL key",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        Result<std::vector<std::pair<std::string, std::string>>> fields =
            ctx.server.keyspace->hashGetAll(argv[1]);
        if (!fields.ok()) {
          return errorFrom(fields.status());
        }
        std::vector<Reply> flat;
        flat.reserve(fields.value().size() * 2);
        for (const auto& [field, value] : fields.value()) {
          flat.push_back(Reply::bulkString(field));
          flat.push_back(Reply::bulkString(value));
        }
        return Reply::array(std::move(flat));
      });

  add(registry, "HDEL", -3, true, "HDEL key field [field ...]",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        const std::vector<std::string> fields(argv.begin() + 2, argv.end());
        Result<std::size_t> removed = ctx.server.keyspace->hashDelete(argv[1], fields);
        return removed.ok() ? Reply::integer(static_cast<std::int64_t>(removed.value()))
                            : errorFrom(removed.status());
      });

  add(registry, "HLEN", 2, false, "HLEN key", [](CommandContext& ctx, const std::vector<std::string>& argv) {
    Result<std::size_t> length = ctx.server.keyspace->hashLength(argv[1]);
    return length.ok() ? Reply::integer(static_cast<std::int64_t>(length.value()))
                       : errorFrom(length.status());
  });

  add(registry, "SADD", -3, true, "SADD key member [member ...]",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        const std::vector<std::string> members(argv.begin() + 2, argv.end());
        Result<std::size_t> added = ctx.server.keyspace->setAdd(argv[1], members);
        return added.ok() ? Reply::integer(static_cast<std::int64_t>(added.value()))
                          : errorFrom(added.status());
      });

  add(registry, "SREM", -3, true, "SREM key member [member ...]",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        const std::vector<std::string> members(argv.begin() + 2, argv.end());
        Result<std::size_t> removed = ctx.server.keyspace->setRemove(argv[1], members);
        return removed.ok() ? Reply::integer(static_cast<std::int64_t>(removed.value()))
                            : errorFrom(removed.status());
      });

  add(registry, "SISMEMBER", 3, false, "SISMEMBER key member",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        Result<bool> contains = ctx.server.keyspace->setContains(argv[1], argv[2]);
        return contains.ok() ? Reply::integer(contains.value() ? 1 : 0) : errorFrom(contains.status());
      });

  add(registry, "SMEMBERS", 2, false, "SMEMBERS key",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        Result<std::vector<std::string>> members = ctx.server.keyspace->setMembers(argv[1]);
        if (!members.ok()) {
          return errorFrom(members.status());
        }
        std::vector<std::string> sorted = members.value();
        std::sort(sorted.begin(), sorted.end());
        return Reply::stringArray(sorted);
      });

  add(registry, "SCARD", 2, false, "SCARD key",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        Result<std::size_t> size = ctx.server.keyspace->setCardinality(argv[1]);
        return size.ok() ? Reply::integer(static_cast<std::int64_t>(size.value())) : errorFrom(size.status());
      });
}

void registerPubSubCommands(CommandRegistry& registry) {
  registry.registerCommand(std::make_unique<SubscribeCommand>());
  registry.registerCommand(std::make_unique<UnsubscribeCommand>());

  add(registry, "PUBLISH", 3, false, "PUBLISH channel message",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        if (ctx.server.pubsub == nullptr) {
          return Reply::error("ERR pub/sub is not available");
        }
        const std::size_t delivered = ctx.server.pubsub->publish(argv[1], argv[2]);
        return Reply::integer(static_cast<std::int64_t>(delivered));
      });

  add(registry, "PUBSUB", -2, false, "PUBSUB CHANNELS",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        if (ctx.server.pubsub == nullptr) {
          return Reply::error("ERR pub/sub is not available");
        }
        const std::string subcommand = toUpper(argv[1]);
        if (subcommand == "CHANNELS") {
          return Reply::stringArray(ctx.server.pubsub->channels());
        }
        if (subcommand == "NUMSUB" && argv.size() >= 3) {
          std::vector<Reply> out;
          for (std::size_t i = 2; i < argv.size(); ++i) {
            out.push_back(Reply::bulkString(argv[i]));
            out.push_back(
                Reply::integer(static_cast<std::int64_t>(ctx.server.pubsub->subscriberCount(argv[i]))));
          }
          return Reply::array(std::move(out));
        }
        return Reply::error("ERR unknown PUBSUB subcommand");
      });
}

void registerAdminCommands(CommandRegistry& registry) {
  registry.registerCommand(std::make_unique<InfoCommand>());

  add(
      registry, "PING", -1, false, "PING [message]",
      [](CommandContext&, const std::vector<std::string>& argv) {
        return argv.size() >= 2 ? Reply::bulkString(argv[1]) : Reply::simpleString("PONG");
      },
      /*is_admin=*/false, /*is_no_auth=*/true);

  add(registry, "ECHO", 2, false, "ECHO message",
      [](CommandContext&, const std::vector<std::string>& argv) { return Reply::bulkString(argv[1]); });

  add(registry, "DBSIZE", 1, false, "DBSIZE", [](CommandContext& ctx, const std::vector<std::string>&) {
    return Reply::integer(static_cast<std::int64_t>(ctx.server.keyspace->size()));
  });

  add(
      registry, "FLUSHALL", -1, true, "FLUSHALL",
      [](CommandContext& ctx, const std::vector<std::string>&) {
        ctx.server.keyspace->clear();
        return Reply::ok();
      },
      /*is_admin=*/true);

  add(
      registry, "QUIT", 1, false, "QUIT",
      [](CommandContext&, const std::vector<std::string>&) { return Reply::ok(); },
      /*is_admin=*/false, /*is_no_auth=*/true);

  add(registry, "COMMAND", -1, false, "COMMAND [DOCS]",
      [&registry](CommandContext&, const std::vector<std::string>&) {
        return Reply::stringArray(registry.commandNames());
      });

  add(
      registry, "CONFIG", -2, false, "CONFIG GET parameter [parameter ...]",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        const std::string subcommand = toUpper(argv[1]);
        if (subcommand != "GET") {
          return Reply::error("ERR only CONFIG GET is supported");
        }

        // redis-cli and redis-benchmark both probe CONFIG GET during start-up
        // and print "Could not fetch server CONFIG" when it comes back empty.
        // Answering with the parameters that actually exist here keeps stock
        // tooling quiet without pretending to support settings we do not have.
        const cache::KeyspaceStats stats = ctx.server.keyspace->stats();
        const std::vector<std::pair<std::string, std::string>> parameters = {
            {"maxmemory-policy", std::string(ctx.server.keyspace->evictionPolicyName())},
            {"maxmemory", std::to_string(stats.memory_bytes)},
            {"save", ""},
            {"appendonly", "no"},
            {"databases", "1"},
        };

        std::vector<Reply> out;
        for (std::size_t i = 2; i < argv.size(); ++i) {
          for (const auto& [key, value] : parameters) {
            if (cache::Keyspace::globMatch(argv[i], key)) {
              out.push_back(Reply::bulkString(key));
              out.push_back(Reply::bulkString(value));
            }
          }
        }
        return Reply::array(std::move(out));
      },
      /*is_admin=*/true);
}

}  // namespace bourse::exec
