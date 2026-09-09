#include "bourse/server/bourse_server.hpp"

#include <charconv>
#include <cstdlib>
#include <sstream>

#include "bourse/auth/crypto.hpp"
#include "bourse/core/clock.hpp"
#include "bourse/core/logger.hpp"
#include "bourse/core/metrics.hpp"
#include "bourse/exec/reply.hpp"
#include "bourse/net/http_codec.hpp"
#include "bourse/net/resp_codec.hpp"
#include "bourse/server/rest_api.hpp"

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

auth::AuthConfig makeAuthConfig(const Config& config) {
  auth::AuthConfig out;
  out.enabled = config.auth_enabled;
  out.session_ttl_ms = config.session_ttl_seconds * 1000;
  out.iterations = config.auth_iterations;
  return out;
}

/// Applies defaults taken from the environment.
///
/// Container platforms -- Render, Railway, Cloud Run, Heroku, Fly -- assign the
/// public port at run time and pass it in $PORT rather than on the command
/// line, because the command line is baked into the image. Reading it here is
/// the whole reason the same image deploys to any of them unchanged.
///
/// Called before argv is parsed, so an explicit flag still wins. A malformed
/// value is an error rather than a silent fallback: a server that quietly binds
/// a port the platform is not routing to looks alive and answers nothing.
Status applyEnvironmentDefaults(Config& config) {
  if (const char* port = std::getenv("PORT"); port != nullptr && *port != '\0') {
    std::size_t parsed = 0;
    if (!parseUnsigned(port, parsed) || parsed == 0 || parsed > 65535) {
      return Status::invalidArgument("PORT must be 1..65535, got '" + std::string(port) + "'");
    }
    config.http_port = static_cast<std::uint16_t>(parsed);
  }
  if (const char* host = std::getenv("BOURSE_HOST"); host != nullptr && *host != '\0') {
    config.host = host;
  }

  // Credentials come from the environment rather than argv wherever possible:
  // `ps` shows every process's command line to every user on the box, and
  // hosting platforms store environment variables as secrets.
  if (const char* enabled = std::getenv("BOURSE_AUTH"); enabled != nullptr && *enabled != '\0') {
    const std::string_view text(enabled);
    if (text == "yes" || text == "true" || text == "1") {
      config.auth_enabled = true;
    } else if (text == "no" || text == "false" || text == "0") {
      config.auth_enabled = false;
    } else {
      return Status::invalidArgument("BOURSE_AUTH must be yes or no, got '" + std::string(text) + "'");
    }
  }
  if (const char* user = std::getenv("BOURSE_ADMIN_USER"); user != nullptr && *user != '\0') {
    config.admin_user = user;
  }
  if (const char* password = std::getenv("BOURSE_ADMIN_PASSWORD"); password != nullptr && *password != '\0') {
    config.admin_password = password;
    // Supplying a password without asking for auth is unambiguous intent, and
    // a server that silently ignored it would be enforcing nothing.
    config.auth_enabled = true;
  }
  return Status::success();
}

}  // namespace

std::string Config::usage() {
  return R"(bourse-server -- exchange, cache and query engine

Usage: bourse-server [options]

  --host <addr>              bind address                     (default 0.0.0.0)
  --port <n>                 RESP port                        (default 6380)
  --http-port <n>            HTTP/dashboard port              (default 8080)
  --no-http                  disable the HTTP listener
  --io-threads <n>           I/O event loops, 0 = auto        (default 0)
  --shards <n>               keyspace shards, power of two    (default 16)
  --maxmemory <size>         eviction budget, e.g. 256mb      (default unlimited)
  --maxmemory-policy <name>  allkeys-lru | allkeys-lfu |
                             allkeys-random | noeviction      (default allkeys-lru)
  --maxmemory-samples <n>    eviction sample size             (default 5)
  --log-level <level>        trace|debug|info|warn|error      (default info)
  --sync-logging             disable the async log consumer

  --dir <path>               data directory                   (default ./bourse-data)
  --appendonly <yes|no>      enable WAL + snapshot recovery    (default no)
  --wal-sync <policy>        never | everysec | always        (default everysec)
  --save-seconds <n>         background snapshot interval,
                             0 disables                       (default 300)

  --auth <yes|no>            require authentication            (default no)
  --admin-user <name>        pre-create this administrator. Optional: with no
                             accounts on file, whoever registers first through
                             the dashboard becomes the administrator, so a
                             deployment needs no seeded credentials.
  --admin-password <pw>      its password. Omit and one is generated and
                             logged once at startup. Prefer the environment
                             variable: argv is visible in `ps`.
  --session-ttl <seconds>    session lifetime                 (default 43200)
  --auth-iterations <n>      PBKDF2 work factor               (default 210000)
  --help                     show this message

Environment (read first; any flag above overrides it):
  PORT                       HTTP/dashboard port. Container platforms assign
                             this at run time, so the image needs no rebuild.
  BOURSE_HOST                bind address
  BOURSE_AUTH                yes|no
  BOURSE_ADMIN_USER          pre-created administrator name; optional
  BOURSE_ADMIN_PASSWORD      its password; setting this implies --auth yes
)";
}

Result<Config> Config::fromArgs(int argc, char** argv) {
  Config config;
  if (const Status environment = applyEnvironmentDefaults(config); !environment.ok()) {
    return environment;
  }

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
    } else if (arg == "--http-port") {
      BOURSE_ASSIGN_OR_RETURN(const std::string text, value_for("--http-port"));
      std::size_t port = 0;
      if (!parseUnsigned(text, port) || port == 0 || port > 65535) {
        return Status::invalidArgument("--http-port must be 1..65535");
      }
      config.http_port = static_cast<std::uint16_t>(port);
    } else if (arg == "--auth") {
      BOURSE_ASSIGN_OR_RETURN(const std::string text, value_for("--auth"));
      if (text == "yes" || text == "true" || text == "1") {
        config.auth_enabled = true;
      } else if (text == "no" || text == "false" || text == "0") {
        config.auth_enabled = false;
      } else {
        return Status::invalidArgument("--auth must be yes or no");
      }
    } else if (arg == "--admin-user") {
      BOURSE_ASSIGN_OR_RETURN(config.admin_user, value_for("--admin-user"));
    } else if (arg == "--admin-password") {
      BOURSE_ASSIGN_OR_RETURN(config.admin_password, value_for("--admin-password"));
      config.auth_enabled = true;
    } else if (arg == "--session-ttl") {
      BOURSE_ASSIGN_OR_RETURN(const std::string text, value_for("--session-ttl"));
      std::size_t seconds = 0;
      if (!parseUnsigned(text, seconds) || seconds == 0) {
        return Status::invalidArgument("--session-ttl must be a positive number of seconds");
      }
      config.session_ttl_seconds = static_cast<std::int64_t>(seconds);
    } else if (arg == "--auth-iterations") {
      BOURSE_ASSIGN_OR_RETURN(const std::string text, value_for("--auth-iterations"));
      std::size_t iterations = 0;
      // 1000 is the floor RFC 8018 recommends; below it the hash is decorative.
      if (!parseUnsigned(text, iterations) || iterations < 1000) {
        return Status::invalidArgument("--auth-iterations must be at least 1000");
      }
      config.auth_iterations = static_cast<std::uint32_t>(iterations);
    } else if (arg == "--no-http") {
      config.enable_http = false;
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
    } else if (arg == "--dir") {
      BOURSE_ASSIGN_OR_RETURN(config.data_dir, value_for("--dir"));
    } else if (arg == "--appendonly") {
      BOURSE_ASSIGN_OR_RETURN(const std::string text, value_for("--appendonly"));
      if (text == "yes" || text == "true" || text == "1") {
        config.append_only = true;
      } else if (text == "no" || text == "false" || text == "0") {
        config.append_only = false;
      } else {
        return Status::invalidArgument("--appendonly must be yes or no");
      }
    } else if (arg == "--wal-sync") {
      BOURSE_ASSIGN_OR_RETURN(config.wal_sync, value_for("--wal-sync"));
      storage::WriteAheadLog::SyncPolicy probe{};
      if (!storage::WriteAheadLog::parseSyncPolicy(config.wal_sync, probe)) {
        return Status::invalidArgument("--wal-sync must be never, everysec or always");
      }
    } else if (arg == "--save-seconds") {
      BOURSE_ASSIGN_OR_RETURN(const std::string text, value_for("--save-seconds"));
      std::size_t seconds = 0;
      if (!parseUnsigned(text, seconds)) {
        return Status::invalidArgument("--save-seconds must be a non-negative integer");
      }
      config.snapshot_interval_seconds = static_cast<std::int64_t>(seconds);
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
      registry_(exec::CommandRegistry::createDefault()),
      auth_(makeAuthConfig(config_)) {
  context_.keyspace = &keyspace_;
  context_.auth = &auth_;
  context_.pubsub = &pubsub_;
  context_.matching_engine = &matching_engine_;
  context_.sql_engine = &sql_engine_;
  context_.started_at_ms = nowMillis();
  context_.version = "1.0.0";

  // Every execution is republished on a pub/sub channel, so `SUBSCRIBE
  // trades:AAPL` in one redis-cli window shows fills produced by orders typed
  // into another. The engine itself knows nothing about pub/sub -- this is the
  // Observer hook doing its job.
  matching_engine_.addTradeObserver([this](const std::string& symbol, const match::Trade& trade) {
    std::string payload;
    payload.reserve(96);
    payload.append(match::toString(trade.aggressor_side))
        .append(" ")
        .append(std::to_string(trade.quantity))
        .append(" @ ")
        .append(match::formatPrice(trade.price))
        .append(" seq=")
        .append(std::to_string(trade.sequence));
    pubsub_.publish("trades:" + symbol, payload);
  });
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

  resp_server_ = std::make_unique<net::TcpServer>(
      options, [this] { return std::make_unique<net::RespCodec>(*registry_, context_); });

  // Recovery must complete before the listener accepts anything: a client that
  // connected mid-replay could observe a keyspace that is only half restored.
  if (config_.append_only) {
    Result<std::size_t> replayed = recover();
    if (!replayed.ok()) {
      return replayed.status();
    }
  }

  // Before the administrator check, so accounts already on file are loaded and
  // a restart does not try to re-create anything.
  BOURSE_TRY(openAccountStore());
  BOURSE_TRY(bootstrapAdministrator());

  BOURSE_TRY(resp_server_->start());
  installPubSubDelivery();
  startBackgroundCron();

  BOURSE_LOG_INFO("RESP endpoint ready on port ", resp_server_->port(), " -- try: redis-cli -p ",
                  resp_server_->port(), " PING");

  if (config_.enable_http) {
    BOURSE_TRY(startHttp());
  }
  return Status::success();
}

std::string BourseServer::snapshotPath() const {
  return config_.data_dir + "/bourse.snapshot";
}

std::string BourseServer::walPath() const {
  return config_.data_dir + "/bourse.wal";
}

std::string BourseServer::accountStorePath() const {
  return config_.data_dir + "/bourse.users";
}

Status BourseServer::openAccountStore() {
  if (!config_.auth_enabled) {
    return Status::success();
  }

  // Deliberately not conditional on --appendonly. Keyspace durability is a
  // performance trade-off a deployment can reasonably decline; losing the
  // accounts people signed up with is not, and an auth server that forgets
  // every user on restart is broken rather than merely fast.
  BOURSE_TRY(File::ensureDirectory(config_.data_dir));
  return auth_.openStore(accountStorePath());
}

Result<std::size_t> BourseServer::recover() {
  BOURSE_TRY(File::ensureDirectory(config_.data_dir));

  // 1. Restore the newest full image.
  if (storage::Snapshot::exists(snapshotPath())) {
    Result<storage::SnapshotStats> loaded = storage::Snapshot::load(keyspace_, snapshotPath());
    if (!loaded.ok()) {
      // Refusing to start beats starting with silently wrong data.
      return Status::corruption("snapshot recovery failed: " + loaded.status().message());
    }
    BOURSE_LOG_INFO("recovered ", loaded.value().keys, " key(s) from snapshot in ",
                    loaded.value().duration_ms, "ms");
  }

  // 2. Replay everything journalled since that image was taken.
  storage::WriteAheadLog::Options options;
  options.path = walPath();
  (void)storage::WriteAheadLog::parseSyncPolicy(config_.wal_sync, options.sync_policy);

  Result<std::unique_ptr<storage::WriteAheadLog>> log = storage::WriteAheadLog::open(std::move(options));
  if (!log.ok()) {
    return log.status();
  }
  wal_ = std::move(log).value();

  // The journal hook stays null during replay. Re-journalling replayed commands
  // would double the log on every restart until it consumed the disk.
  std::size_t applied = 0;
  std::uint64_t truncated = 0;
  Result<std::size_t> replayed = wal_->replay(
      [&](const storage::WalRecord& record) {
        // Replay reapplies commands this server already accepted and
        // journalled, so it runs as an administrator. Re-checking permissions
        // here would let a role change silently drop history on restart.
        exec::CommandContext command_context{context_, nullptr,
                                             auth::Principal{"(wal-replay)", auth::Role::kAdmin}, "local"};
        const exec::Reply reply = registry_->dispatch(command_context, record.argv);
        if (reply.isError()) {
          BOURSE_LOG_WARN("WAL replay: record ", record.sequence, " failed: ", reply.text());
        } else {
          ++applied;
        }
      },
      &truncated);
  if (!replayed.ok()) {
    return replayed.status();
  }

  if (replayed.value() > 0 || truncated > 0) {
    BOURSE_LOG_INFO("replayed ", applied, " of ", replayed.value(), " WAL record(s)",
                    truncated > 0 ? " after discarding a torn tail" : "");
  }

  // Only now is it safe to start journalling live traffic.
  context_.journal = [this](const std::vector<std::string>& argv) {
    const Status status = wal_->append(argv);
    if (!status.ok()) {
      BOURSE_LOG_ERROR("WAL append failed: ", status.toString());
    }
  };

  BOURSE_LOG_INFO("persistence enabled: dir=", config_.data_dir, " sync=", config_.wal_sync);
  return applied;
}

Status BourseServer::bootstrapAdministrator() {
  if (!config_.auth_enabled) {
    return Status::success();
  }

  // No configured administrator is the normal case, not an error. Accounts are
  // created by people registering, and the first registration on an empty
  // store is made an administrator -- so a public deployment ships with no
  // credentials at all, and there is nothing published to guess.
  if (config_.admin_user.empty()) {
    return Status::success();
  }
  if (auth_.hasUser(config_.admin_user)) {
    return Status::success();
  }

  std::string password = config_.admin_password;
  bool generated = false;
  if (password.empty()) {
    // Deploying with a default password is how demo servers end up owned. A
    // generated one is printed exactly once, at startup, and never stored in
    // the repository, the image, or the platform's environment.
    Result<std::string> random = auth::randomToken(12);
    if (!random.ok()) {
      return random.status();
    }
    password = std::move(random).value();
    generated = true;
  }

  const Status added = auth_.addUser(config_.admin_user, password, auth::Role::kAdmin);
  if (!added.ok()) {
    return added;
  }

  if (generated) {
    BOURSE_LOG_WARN("=====================================================================");
    BOURSE_LOG_WARN("  Generated administrator credentials -- shown once, not stored:");
    BOURSE_LOG_WARN("    username: ", config_.admin_user);
    BOURSE_LOG_WARN("    password: ", password);
    BOURSE_LOG_WARN("  Set --admin-password or $BOURSE_ADMIN_PASSWORD to choose your own.");
    BOURSE_LOG_WARN("=====================================================================");
  } else {
    BOURSE_LOG_INFO("administrator '", config_.admin_user, "' created from configuration");
  }

  return Status::success();
}

Status BourseServer::persistSnapshot() {
  if (!config_.append_only) {
    return Status::success();
  }
  Result<storage::SnapshotStats> saved = storage::Snapshot::save(keyspace_, snapshotPath());
  if (!saved.ok()) {
    BOURSE_LOG_ERROR("snapshot failed: ", saved.status().toString());
    return saved.status();
  }
  BOURSE_LOG_INFO("snapshot written: ", saved.value().keys, " key(s), ", saved.value().bytes, " bytes, ",
                  saved.value().duration_ms, "ms");

  // The snapshot now covers everything the log described, so the log can start
  // over. This is what stops the WAL growing without bound.
  if (wal_) {
    BOURSE_TRY(wal_->reset());
  }
  return Status::success();
}

Status BourseServer::startHttp() {
  router_.use(net::makeMetricsMiddleware());
  // CORS first: a request rejected by auth still has to carry the headers a
  // browser needs in order to read the 401 rather than reporting a network
  // error, and pre-flights must be answered before any credential check.
  router_.use(net::makeCorsMiddleware());
  router_.use(makeRestAuthMiddleware(context_));
  buildAuthApi(router_, context_);
  buildTwoFactorApi(router_, context_);
  buildRestApi(router_, context_, *registry_);

  net::ServerOptions options;
  options.host = config_.host;
  options.port = config_.http_port;
  // Two I/O threads is plenty: the dashboard polls once a second and the REST
  // endpoints delegate straight to the same command registry.
  options.io_threads = 2;
  options.name = "bourse-http";

  http_server_ =
      std::make_unique<net::TcpServer>(options, [this] { return std::make_unique<net::HttpCodec>(router_); });
  BOURSE_TRY(http_server_->start());

  // Its own acceptor thread, so a browser holding a keep-alive connection open
  // can never delay the RESP acceptor.
  http_thread_ = std::thread([this] { http_server_->runForever(); });

  BOURSE_LOG_INFO("HTTP endpoint ready on port ", http_server_->port(),
                  " -- dashboard at http://localhost:", http_server_->port(), "/  (", router_.routeCount(),
                  " routes)");
  return Status::success();
}

void BourseServer::stopHttp() {
  if (!http_server_) {
    return;
  }
  http_server_->acceptorLoop().stop();
  if (http_thread_.joinable()) {
    http_thread_.join();
  }
  http_server_->stop();
}

void BourseServer::installPubSubDelivery() {
  // The publisher does not know how to reach a subscriber's socket, and must
  // not: it hands an id and a payload to the server, which resolves the id
  // under its own lock and hops to the owning event loop. This indirection is
  // what makes publish-during-disconnect safe.
  pubsub_.setDelivery(
      [this](std::uint64_t connection_id, const std::string& channel, const std::string& payload) {
        exec::Reply message =
            exec::Reply::array({exec::Reply::bulkString("message"), exec::Reply::bulkString(channel),
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
    // Lazy expiry alone leaks: a session nobody ever presents again is never
    // looked up, so it is never noticed as expired.
    auth_.sweepExpired(nowMillis());
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

  if (config_.append_only) {
    // Group-commit tick for the `everysec` policy: one fsync per second
    // regardless of write rate, rather than one per command.
    resp_server_->acceptorLoop().scheduleEvery(200, [this] {
      if (wal_) {
        const Status status = wal_->maybeSync();
        if (!status.ok()) {
          BOURSE_LOG_ERROR("WAL sync failed: ", status.toString());
        }
      }
    });

    if (config_.snapshot_interval_seconds > 0) {
      resp_server_->acceptorLoop().scheduleEvery(config_.snapshot_interval_seconds * 1000,
                                                 [this] { (void)persistSnapshot(); });
    }
  }
}

void BourseServer::run() {
  if (!resp_server_) {
    BOURSE_LOG_ERROR("run() called before a successful start()");
    return;
  }
  resp_server_->runForever();
  stopHttp();
  resp_server_->stop();

  // Take a final image on a clean shutdown so the next start replays nothing.
  if (config_.append_only) {
    if (wal_) {
      (void)wal_->sync();
    }
    (void)persistSnapshot();
  }

  BOURSE_LOG_INFO("shutdown complete");
  Logger::instance().flush();
}

void BourseServer::requestShutdown() {
  shutdown_requested_.store(true, std::memory_order_release);
  if (http_server_) {
    http_server_->acceptorLoop().stop();
  }
  if (resp_server_) {
    resp_server_->acceptorLoop().stop();
  }
}

std::uint16_t BourseServer::respPort() const noexcept {
  return resp_server_ ? resp_server_->port() : 0;
}

std::uint16_t BourseServer::httpPort() const noexcept {
  return http_server_ ? http_server_->port() : 0;
}

}  // namespace bourse::server
