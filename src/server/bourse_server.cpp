#include "bourse/server/bourse_server.hpp"

#include <charconv>
#include <sstream>

#include "bourse/core/clock.hpp"
#include "bourse/core/logger.hpp"
#include "bourse/core/metrics.hpp"
#include "bourse/exec/reply.hpp"
#include "bourse/net/resp_codec.hpp"

namespace bourse::server {
namespace {

bool parseSize(std::string_view text, std::size_t& out) {
  // Accepts a plain byte count or a `kb`/`mb`/`gb` suffix, because a maxmemory
  // flag that only takes bytes is a usability trap.
  std::size_t multiplier = 1;
  std::string_view digits = text;
  const auto ends_with = [&](std::string_view suffix) {
    return text.size() > suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
  };
  if (ends_with("kb") || ends_with("KB")) {
    multiplier = 1024;
    digits = text.substr(0, text.size() - 2);
  } else if (ends_with("mb") || ends_with("MB")) {
    multiplier = 1024 * 1024;
    digits = text.substr(0, text.size() - 2);
  } else if (ends_with("gb") || ends_with("GB")) {
    multiplier = 1024ULL * 1024 * 1024;
    digits = text.substr(0, text.size() - 2);
  }

  std::uint64_t value = 0;
  const char* begin = digits.data();
  const char* end = digits.data() + digits.size();
  const std::from_chars_result result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  out = static_cast<std::size_t>(value) * multiplier;
  return true;
}

bool parseUnsigned(std::string_view text, std::size_t& out) {
  std::uint64_t value = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const std::from_chars_result result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    return false;
  }
  out = static_cast<std::size_t>(value);
  return true;
}

}  // namespace

std::string Config::usage() {
  return R"(bourse-server -- exchange, cache and query engine

Usage: bourse-server [options]

  --host <addr>              bind address                     (default 0.0.0.0)
  --port <n>                 RESP port                        (default 6380)
  --io-threads <n>           I/O event loops, 0 = auto        (default 0)
  --shards <n>               keyspace shards, power of two    (default 16)
  --maxmemory <size>         eviction budget, e.g. 256mb      (default unlimited)
  --maxmemory-policy <name>  allkeys-lru | allkeys-lfu |
                             allkeys-random | noeviction      (default allkeys-lru)
  --maxmemory-samples <n>    eviction sample size             (default 5)
  --log-level <level>        trace|debug|info|warn|error      (default info)
  --sync-logging             disable the async log consumer
  --help                     show this message
)";
}

Result<Config> Config::fromArgs(int argc, char** argv) {
  Config config;

  const auto need_value = [&](int& i, std::string_view flag) -> Result<std::string> {
    if (i + 1 >= argc) {
      return Status::invalidArgument(std::string(flag) + " requires a value");
    }
    return std::string(argv[++i]);
  };

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];

    // Support --key=value as well as --key value.
    std::string inline_value;
    const std::size_t equals = arg.find('=');
    if (equals != std::string_view::npos) {
      inline_value = std::string(arg.substr(equals + 1));
      arg = arg.substr(0, equals);
    }
    const auto value_for = [&](std::string_view flag) -> Result<std::string> {
      if (!inline_value.empty()) {
        return inline_value;
      }
      return need_value(i, flag);
    };

    if (arg == "--help" || arg == "-h") {
      return Status(ErrorCode::kInvalidArgument, "help");
    }
    if (arg == "--host") {
      BOURSE_ASSIGN_OR_RETURN(config.host, value_for("--host"));
    } else if (arg == "--port") {
      BOURSE_ASSIGN_OR_RETURN(const std::string text, value_for("--port"));
      std::size_t port = 0;
      if (!parseUnsigned(text, port) || port == 0 || port > 65535) {
        return Status::invalidArgument("--port must be 1..65535");
      }
      config.resp_port = static_cast<std::uint16_t>(port);
    } else if (arg == "--io-threads") {
      BOURSE_ASSIGN_OR_RETURN(const std::string text, value_for("--io-threads"));
      if (!parseUnsigned(text, config.io_threads)) {
        return Status::invalidArgument("--io-threads must be a non-negative integer");
      }
    } else if (arg == "--shards") {
      BOURSE_ASSIGN_OR_RETURN(const std::string text, value_for("--shards"));
      if (!parseUnsigned(text, config.shard_count) || config.shard_count == 0) {
        return Status::invalidArgument("--shards must be a positive integer");
      }
    } else if (arg == "--maxmemory") {
      BOURSE_ASSIGN_OR_RETURN(const std::string text, value_for("--maxmemory"));
      if (!parseSize(text, config.max_memory_bytes)) {
        return Status::invalidArgument("--maxmemory must be a size, e.g. 268435456 or 256mb");
      }
    } else if (arg == "--maxmemory-policy") {
      BOURSE_ASSIGN_OR_RETURN(config.eviction_policy, value_for("--maxmemory-policy"));
    } else if (arg == "--maxmemory-samples") {
      BOURSE_ASSIGN_OR_RETURN(const std::string text, value_for("--maxmemory-samples"));
      if (!parseUnsigned(text, config.eviction_sample_size) || config.eviction_sample_size == 0) {
        return Status::invalidArgument("--maxmemory-samples must be a positive integer");
      }
    } else if (arg == "--log-level") {
      BOURSE_ASSIGN_OR_RETURN(config.log_level, value_for("--log-level"));
    } else if (arg == "--sync-logging") {
      config.async_logging = false;
    } else {
      return Status::invalidArgument("unknown option '" + std::string(arg) + "'");
    }
  }

  return config;
}

std::string Config::describe() const {
  std::ostringstream out;
  out << "host=" << host << " port=" << resp_port << " io_threads=" << io_threads << " shards=" << shard_count
      << " maxmemory=" << max_memory_bytes << " policy=" << eviction_policy << " log_level=" << log_level;
  return out.str();
}

// ---------------------------------------------------------------------------

BourseServer::BourseServer(Config config)
    : config_(std::move(config)),
      keyspace_(cache::KeyspaceOptions{config_.shard_count, config_.max_memory_bytes,
                                       config_.eviction_sample_size, config_.eviction_policy}),
      registry_(exec::CommandRegistry::createDefault()) {
  context_.keyspace = &keyspace_;
  context_.pubsub = &pubsub_;
  context_.started_at_ms = nowMillis();
  context_.version = "1.0.0";
}

BourseServer::~BourseServer() {
  if (resp_server_) {
    resp_server_->stop();
  }
  Logger::instance().flush();
}

Status BourseServer::start() {
  Logger::instance().setLevel(parseLogLevel(config_.log_level, LogLevel::kInfo));
  Logger::instance().setAsync(config_.async_logging);

  BOURSE_LOG_INFO("bourse ", context_.version, " starting: ", config_.describe());
  BOURSE_TRY(Socket::initializeNetworking());

  net::ServerOptions options;
  options.host = config_.host;
  options.port = config_.resp_port;
  options.io_threads = config_.io_threads;
  options.name = "bourse-resp";

  resp_server_ = std::make_unique<net::TcpServer>(options, [this] {
    return std::make_unique<net::RespCodec>(*registry_, context_);
  });

  BOURSE_TRY(resp_server_->start());
  installPubSubDelivery();
  startBackgroundCron();

  BOURSE_LOG_INFO("RESP endpoint ready on port ", resp_server_->port(), " -- try: redis-cli -p ",
                  resp_server_->port(), " PING");
  return Status::success();
}

void BourseServer::installPubSubDelivery() {
  // The publisher does not know how to reach a subscriber's socket, and must
  // not: it hands an id and a payload to the server, which resolves the id
  // under its own lock and hops to the owning event loop. This indirection is
  // what makes publish-during-disconnect safe.
  pubsub_.setDelivery([this](std::uint64_t connection_id, const std::string& channel,
                             const std::string& payload) {
    exec::Reply message = exec::Reply::array({exec::Reply::bulkString("message"),
                                              exec::Reply::bulkString(channel),
                                              exec::Reply::bulkString(payload)});
    resp_server_->sendTo(connection_id, message.toResp());
  });
}

void BourseServer::startBackgroundCron() {
  // Active expiry and budget enforcement run on the acceptor loop, which is
  // otherwise idle between connection storms -- no extra thread needed.
  resp_server_->acceptorLoop().scheduleEvery(config_.expire_cycle_ms, [this] {
    const std::size_t expired = keyspace_.activeExpireCycle(config_.expire_sample_per_shard);
    const std::size_t evicted = keyspace_.enforceMemoryBudget();
    if (expired > 0) {
      MetricsRegistry::instance().counter("bourse_expired_keys_total").increment(expired);
    }
    if (evicted > 0) {
      MetricsRegistry::instance().counter("bourse_evicted_keys_total").increment(evicted);
    }
  });

  resp_server_->acceptorLoop().scheduleEvery(1000, [this] {
    const cache::KeyspaceStats stats = keyspace_.stats();
    MetricsRegistry& metrics = MetricsRegistry::instance();
    metrics.gauge("bourse_keys").set(static_cast<std::int64_t>(stats.keys));
    metrics.gauge("bourse_memory_bytes").set(static_cast<std::int64_t>(stats.memory_bytes));
    metrics.gauge("bourse_keyspace_hits").set(static_cast<std::int64_t>(stats.hits));
    metrics.gauge("bourse_keyspace_misses").set(static_cast<std::int64_t>(stats.misses));

    if (shutdown_requested_.load(std::memory_order_acquire)) {
      resp_server_->acceptorLoop().stop();
    }
  });

  // A short-period tick so Ctrl-C is honoured promptly rather than after the
  // one-second stats interval.
  resp_server_->acceptorLoop().scheduleEvery(100, [this] {
    if (shutdown_requested_.load(std::memory_order_acquire)) {
      resp_server_->acceptorLoop().stop();
    }
  });
}

void BourseServer::run() {
  if (!resp_server_) {
    BOURSE_LOG_ERROR("run() called before a successful start()");
    return;
  }
  resp_server_->runForever();
  resp_server_->stop();
  BOURSE_LOG_INFO("shutdown complete");
  Logger::instance().flush();
}

void BourseServer::requestShutdown() {
  shutdown_requested_.store(true, std::memory_order_release);
  if (resp_server_) {
    resp_server_->acceptorLoop().stop();
  }
}

std::uint16_t BourseServer::respPort() const noexcept {
  return resp_server_ ? resp_server_->port() : 0;
}

}  // namespace bourse::server
