#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "bourse/cache/keyspace.hpp"
#include "bourse/exec/command.hpp"
#include "bourse/exec/pubsub.hpp"
#include "bourse/net/server.hpp"

namespace bourse::server {

struct Config {
  std::string host = "0.0.0.0";
  std::uint16_t resp_port = 6380;

  /// 0 selects hardware_concurrency.
  std::size_t io_threads = 0;
  std::size_t shard_count = 16;
  std::size_t max_memory_bytes = 0;
  std::size_t eviction_sample_size = 5;
  std::string eviction_policy = "allkeys-lru";

  std::string log_level = "info";
  bool async_logging = true;

  /// How often the active-expiry cycle runs, and how many keys it samples per
  /// shard per pass. Together these bound the CPU that background expiry can
  /// consume regardless of keyspace size.
  std::int64_t expire_cycle_ms = 100;
  std::size_t expire_sample_per_shard = 20;

  /// Parses `--key value` and `--key=value` argv pairs.
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
  [[nodiscard]] exec::CommandRegistry& registry() noexcept { return *registry_; }
  [[nodiscard]] exec::ServerContext& context() noexcept { return context_; }
  [[nodiscard]] std::uint16_t respPort() const noexcept;
  [[nodiscard]] const Config& config() const noexcept { return config_; }

 private:
  void installPubSubDelivery();
  void startBackgroundCron();

  Config config_;
  cache::Keyspace keyspace_;
  exec::PubSub pubsub_;
  std::unique_ptr<exec::CommandRegistry> registry_;
  exec::ServerContext context_;
  std::unique_ptr<net::TcpServer> resp_server_;
  std::atomic<bool> shutdown_requested_{false};
};

}  // namespace bourse::server
