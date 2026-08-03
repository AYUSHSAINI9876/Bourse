#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "bourse/cache/keyspace.hpp"
#include "bourse/auth/auth_service.hpp"
#include "bourse/exec/command.hpp"
#include "bourse/exec/pubsub.hpp"
#include "bourse/match/matching_engine.hpp"
#include "bourse/net/router.hpp"
#include "bourse/net/server.hpp"
#include "bourse/sql/engine.hpp"
#include "bourse/storage/snapshot.hpp"
#include "bourse/storage/wal.hpp"

namespace bourse::server {

struct Config {
  std::string host = "0.0.0.0";
  std::uint16_t resp_port = 6380;
  /// HTTP/REST + dashboard. Served by a second acceptor on its own thread so a
  /// slow browser poll can never delay RESP traffic.
  std::uint16_t http_port = 8080;
  bool enable_http = true;

  /// 0 selects hardware_concurrency.
  std::size_t io_threads = 0;
  std::size_t shard_count = 16;
  std::size_t max_memory_bytes = 0;
  std::size_t eviction_sample_size = 5;
  std::string eviction_policy = "allkeys-lru";

  std::string log_level = "info";
  bool async_logging = true;

  /// Persistence. Off by default so a throwaway run leaves nothing behind;
  /// `--appendonly yes` turns on the WAL and snapshot pair.
  std::string data_dir = "./bourse-data";
  bool append_only = false;
  std::string wal_sync = "everysec";
  /// Background snapshot interval in seconds. 0 disables periodic snapshots
  /// (one is still written on clean shutdown).
  std::int64_t snapshot_interval_seconds = 300;

  /// How often the active-expiry cycle runs, and how many keys it samples per
  /// shard per pass. Together these bound the CPU that background expiry can
  /// consume regardless of keyspace size.
  std::int64_t expire_cycle_ms = 100;
  std::size_t expire_sample_per_shard = 20;

  /// Authentication. Off by default so a local run and the existing smoke
  /// suites need no credentials; a public deployment turns it on.
  bool auth_enabled = false;
  /// Bootstrap administrator, created at startup when auth is enabled. An
  /// empty password means "generate one and log it once", which is how the
  /// server can be deployed without a secret being committed anywhere.
  std::string admin_user = "admin";
  std::string admin_password;

  /// Optional read-only account, seeded at startup like the administrator.
  ///
  /// Exists so a public demo has something to hand out. Users created at
  /// runtime live only in memory and vanish on restart, so a README could not
  /// advertise them; this one is configuration and comes back every time.
  /// Empty (the default) creates nothing.
  std::string demo_user;
  std::string demo_password;
  std::int64_t session_ttl_seconds = 12 * 60 * 60;
  std::uint32_t auth_iterations = 210000;

  /// Parses `--key value` and `--key=value` argv pairs.
  ///
  /// Environment variables are read first and argv overrides them, so a
  /// container image can carry a fixed command line and still take its port
  /// from the platform: $PORT sets `http_port`, $BOURSE_HOST sets `host`.
  static Result<Config> fromArgs(int argc, char** argv);
  [[nodiscard]] std::string describe() const;
  [[nodiscard]] static std::string usage();
};

/// Wires every layer together and owns their lifetimes.
///
/// Construction order matters and is enforced by member order: the keyspace
/// must outlive the command registry that points at it, which must outlive the
/// codecs the TCP server hands to connections. Declaring the TcpServer last
/// means destruction runs in exactly the reverse order, so no connection can
/// still be executing a command against a half-destroyed keyspace.
class BourseServer {
 public:
  explicit BourseServer(Config config);
  ~BourseServer();

  BourseServer(const BourseServer&) = delete;
  BourseServer& operator=(const BourseServer&) = delete;

  Status start();
  /// Blocks on the acceptor loop until `requestShutdown()`.
  void run();
  void requestShutdown();

  [[nodiscard]] cache::Keyspace& keyspace() noexcept { return keyspace_; }
  [[nodiscard]] exec::PubSub& pubsub() noexcept { return pubsub_; }
  [[nodiscard]] match::MatchingEngine& matchingEngine() noexcept { return matching_engine_; }
  [[nodiscard]] sql::Engine& sqlEngine() noexcept { return sql_engine_; }
  [[nodiscard]] exec::CommandRegistry& registry() noexcept { return *registry_; }
  [[nodiscard]] exec::ServerContext& context() noexcept { return context_; }
  [[nodiscard]] auth::AuthService& auth() noexcept { return auth_; }
  [[nodiscard]] std::uint16_t respPort() const noexcept;
  [[nodiscard]] std::uint16_t httpPort() const noexcept;
  [[nodiscard]] const Config& config() const noexcept { return config_; }
  [[nodiscard]] net::Router& router() noexcept { return router_; }

 private:
  void installPubSubDelivery();
  void startBackgroundCron();
  Status startHttp();
  void stopHttp();
  /// Loads the newest snapshot, then replays only the WAL records written
  /// after it. Returns the number of commands replayed.
  Result<std::size_t> recover();
  Status persistSnapshot();

  /// Creates the bootstrap administrator. Generates and logs a password when
  /// none was configured.
  Status bootstrapAdministrator();

  [[nodiscard]] std::string snapshotPath() const;
  [[nodiscard]] std::string walPath() const;

  Config config_;
  cache::Keyspace keyspace_;
  exec::PubSub pubsub_;
  match::MatchingEngine matching_engine_;
  sql::Engine sql_engine_;
  std::unique_ptr<exec::CommandRegistry> registry_;
  auth::AuthService auth_;
  exec::ServerContext context_;
  std::unique_ptr<storage::WriteAheadLog> wal_;
  net::Router router_;
  std::unique_ptr<net::TcpServer> resp_server_;
  std::unique_ptr<net::TcpServer> http_server_;
  std::thread http_thread_;
  std::atomic<bool> shutdown_requested_{false};
};

}  // namespace bourse::server
