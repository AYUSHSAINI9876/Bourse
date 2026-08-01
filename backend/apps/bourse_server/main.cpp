#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "bourse/server/bourse_server.hpp"

namespace {

/// The signal handler may only touch async-signal-safe state. It stores into an
/// atomic and writes one byte to the loop's wakeup descriptor -- both safe --
/// rather than doing any real shutdown work in signal context.
std::atomic<bourse::server::BourseServer*> g_server{nullptr};

extern "C" void handleTerminationSignal(int /*signal*/) {
  bourse::server::BourseServer* server = g_server.load(std::memory_order_acquire);
  if (server != nullptr) {
    server->requestShutdown();
  }
}

}  // namespace

int main(int argc, char** argv) {
  bourse::Result<bourse::server::Config> config = bourse::server::Config::fromArgs(argc, argv);
  if (!config.ok()) {
    if (config.status().message() == "help") {
      std::fputs(bourse::server::Config::usage().c_str(), stdout);
      return 0;
    }
    std::fprintf(stderr, "bourse-server: %s\n\n", config.status().message().c_str());
    std::fputs(bourse::server::Config::usage().c_str(), stderr);
    return 2;
  }

  bourse::server::BourseServer server(std::move(config).value());
  g_server.store(&server, std::memory_order_release);

  std::signal(SIGINT, handleTerminationSignal);
#if defined(SIGTERM)
  std::signal(SIGTERM, handleTerminationSignal);
#endif

  const bourse::Status started = server.start();
  if (!started.ok()) {
    std::fprintf(stderr, "bourse-server: failed to start: %s\n", started.toString().c_str());
    g_server.store(nullptr, std::memory_order_release);
    return 1;
  }

  server.run();
  g_server.store(nullptr, std::memory_order_release);
  return 0;
}
