#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bourse/exec/reply.hpp"

namespace bourse::cache {
class Keyspace;
}
namespace bourse::match {
class MatchingEngine;
}
namespace bourse::sql {
class Engine;
}
namespace bourse::net {
class Connection;
}

namespace bourse::exec {

class PubSub;

/// Everything a command is allowed to reach. Passed by reference so commands
/// remain stateless singletons -- one instance per verb for the whole process,
/// registered once at startup.
struct ServerContext {
  cache::Keyspace* keyspace = nullptr;
  PubSub* pubsub = nullptr;
  match::MatchingEngine* matching_engine = nullptr;
  sql::Engine* sql_engine = nullptr;

  /// Write-ahead journal hook. Null when persistence is disabled.
  ///
  /// Installed by the server and invoked by the registry, not by individual
  /// commands. Putting it in one place means a newly added write verb is
  /// journalled automatically -- it only has to answer `isWrite()` honestly --
  /// and that RESP and REST can never drift apart on what gets persisted.
  std::function<void(const std::vector<std::string>&)> journal;

  std::int64_t started_at_ms = 0;
  std::string version = "1.0.0";
};

struct CommandContext {
  ServerContext& server;
  /// Null when the command arrives over REST rather than RESP. Commands that
  /// genuinely need a connection (SUBSCRIBE) check for this and error out.
  net::Connection* connection = nullptr;
};

/// Abstract base for every verb.
///
/// The Command pattern here is not decoration: it is what makes the dispatch
/// table data rather than a 60-case switch. Arity and name live on the object,
/// so validation happens once in the registry instead of being re-implemented
/// (and mis-implemented) at the top of every handler.
class Command {
 public:
  virtual ~Command() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// Expected argument count *including* the command name. A negative value
  /// means "at least |arity|", matching Redis's own convention.
  [[nodiscard]] virtual int arity() const noexcept = 0;

  /// True for commands that mutate state. The AOF writer uses this to decide
  /// what to journal, so a new read-only command is never accidentally logged.
  [[nodiscard]] virtual bool isWrite() const noexcept { return false; }

  [[nodiscard]] virtual std::string_view summary() const noexcept { return ""; }

  virtual Reply execute(CommandContext& context, const std::vector<std::string>& argv) = 0;
};

using CommandPtr = std::unique_ptr<Command>;

/// Adapter that turns a callable into a `Command`.
///
/// Most verbs are a handful of lines with no state of their own, and giving
/// each one a named class would add roughly forty near-identical class
/// definitions without adding any information. Verbs that *do* carry state or
/// non-trivial option parsing -- SET with its EX/PX/NX/XX flags, SUBSCRIBE,
/// INFO -- are written as explicit classes in commands.cpp, because there the
/// class boundary is doing real work.
class LambdaCommand final : public Command {
 public:
  using Handler = std::function<Reply(CommandContext&, const std::vector<std::string>&)>;

  LambdaCommand(std::string name, int arity, bool is_write, std::string summary, Handler handler)
      : name_(std::move(name)),
        summary_(std::move(summary)),
        handler_(std::move(handler)),
        arity_(arity),
        is_write_(is_write) {}

  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  [[nodiscard]] int arity() const noexcept override { return arity_; }
  [[nodiscard]] bool isWrite() const noexcept override { return is_write_; }
  [[nodiscard]] std::string_view summary() const noexcept override { return summary_; }

  Reply execute(CommandContext& context, const std::vector<std::string>& argv) override {
    return handler_(context, argv);
  }

 private:
  std::string name_;
  std::string summary_;
  Handler handler_;
  int arity_;
  bool is_write_;
};

/// Name -> Command dispatch table.
///
/// Lookup is case-insensitive (clients send `get`, `GET` and `Get`
/// interchangeably) and happens through a transparent comparator so no
/// temporary uppercase string is allocated per request.
class CommandRegistry {
 public:
  CommandRegistry();

  void registerCommand(CommandPtr command);

  [[nodiscard]] const Command* find(std::string_view name) const;

  /// Resolves, validates arity, and executes. Unknown verbs and arity errors
  /// come back as RESP errors rather than exceptions.
  Reply dispatch(CommandContext& context, const std::vector<std::string>& argv) const;

  [[nodiscard]] std::vector<std::string> commandNames() const;
  [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }

  /// Builds the registry with every built-in verb installed.
  static std::unique_ptr<CommandRegistry> createDefault();

 private:
  struct CaseInsensitiveLess {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept;
  };

  std::map<std::string, CommandPtr, CaseInsensitiveLess> commands_;
};

/// Installs the string/generic/TTL verbs.
void registerKeyspaceCommands(CommandRegistry& registry);
/// Installs LPUSH/RPUSH/LPOP/RPOP/LRANGE/LLEN.
void registerListCommands(CommandRegistry& registry);
/// Installs HSET/HGET/HGETALL/HDEL/HLEN and SADD/SREM/SMEMBERS/SISMEMBER/SCARD.
void registerCollectionCommands(CommandRegistry& registry);
/// Installs PING/ECHO/INFO/DBSIZE/FLUSHALL/COMMAND/QUIT and the exchange verbs.
void registerAdminCommands(CommandRegistry& registry);
/// Installs SUBSCRIBE/UNSUBSCRIBE/PUBLISH.
void registerPubSubCommands(CommandRegistry& registry);
/// Installs ORDER/CANCEL/AMEND/BOOK/TRADES/SYMBOLS/EXCHANGE. These are no-ops
/// that answer with an error unless a MatchingEngine is present in the context.
void registerExchangeCommands(CommandRegistry& registry);
/// Installs SQL/EXPLAIN/TABLES/DESCRIBE.
void registerSqlCommands(CommandRegistry& registry);

}  // namespace bourse::exec
