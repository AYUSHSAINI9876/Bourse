#include <algorithm>
#include <cctype>
#include <string>

#include "bourse/auth/auth_service.hpp"
#include "bourse/core/logger.hpp"
#include "bourse/exec/command.hpp"

namespace bourse::exec {

bool CommandRegistry::CaseInsensitiveLess::operator()(std::string_view a, std::string_view b) const noexcept {
  // Hand-rolled rather than transforming both sides to a temporary uppercase
  // string: this runs once per request, and allocating two strings to compare
  // three characters is exactly the kind of waste that shows up at 100k ops/s.
  const std::size_t common = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < common; ++i) {
    const auto lhs = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(a[i])));
    const auto rhs = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(b[i])));
    if (lhs != rhs) {
      return lhs < rhs;
    }
  }
  return a.size() < b.size();
}

CommandRegistry::CommandRegistry() = default;

void CommandRegistry::registerCommand(CommandPtr command) {
  if (!command) {
    return;
  }
  std::string name(command->name());
  commands_.insert_or_assign(std::move(name), std::move(command));
}

const Command* CommandRegistry::find(std::string_view name) const {
  auto it = commands_.find(name);
  return it == commands_.end() ? nullptr : it->second.get();
}

Reply CommandRegistry::dispatch(CommandContext& context, const std::vector<std::string>& argv) const {
  if (argv.empty()) {
    return Reply::error("ERR empty command");
  }

  const Command* command = find(argv[0]);
  if (command == nullptr) {
    std::string message = "ERR unknown command '" + argv[0] + "'";
    return Reply::error(std::move(message));
  }

  // Arity is validated centrally so no handler has to repeat it. Negative
  // arity means "at least this many", matching the Redis convention.
  const int arity = command->arity();
  const auto argc = static_cast<int>(argv.size());
  const bool arity_ok = arity >= 0 ? argc == arity : argc >= -arity;
  if (!arity_ok) {
    return Reply::error("ERR wrong number of arguments for '" + std::string(command->name()) + "' command");
  }

  // Permissions are checked in exactly one place, for the same reason the
  // journal hook is: a verb added tomorrow is covered by default, and RESP and
  // REST cannot drift apart on what a role is allowed to do. A handler that
  // forgot its own check would be a silent hole; there is no handler-level
  // check to forget.
  if (context.server.auth != nullptr && context.server.auth->enabled()) {
    const auth::Role required = command->requiredRole();
    if (!context.principal.can(required)) {
      // Distinguishing the two is deliberate. NOAUTH tells a client to
      // authenticate and retry; NOPERM tells it that retrying will not help.
      // Collapsing them into one message sends clients into login loops.
      if (!context.principal.authenticated()) {
        return Reply::error("NOAUTH Authentication required.");
      }
      return Reply::error("NOPERM this user has no permissions to run the '" + std::string(command->name()) +
                          "' command");
    }
  }

  Reply reply = const_cast<Command*>(command)->execute(context, argv);

  // Journal after the fact, and only on success. Logging before execution
  // would persist commands that turned out to be rejected, and replaying those
  // would produce a keyspace the original server never had.
  if (context.server.journal && command->isWrite() && !reply.isError()) {
    context.server.journal(argv);
  }
  return reply;
}

std::vector<std::string> CommandRegistry::commandNames() const {
  std::vector<std::string> names;
  names.reserve(commands_.size());
  for (const auto& [name, command] : commands_) {
    names.push_back(name);
  }
  return names;
}

std::unique_ptr<CommandRegistry> CommandRegistry::createDefault() {
  auto registry = std::make_unique<CommandRegistry>();
  registerKeyspaceCommands(*registry);
  registerListCommands(*registry);
  registerCollectionCommands(*registry);
  registerPubSubCommands(*registry);
  registerExchangeCommands(*registry);
  registerSqlCommands(*registry);
  registerAuthCommands(*registry);
  registerAdminCommands(*registry);
  BOURSE_LOG_INFO("command registry initialised with ", registry->size(), " verbs");
  return registry;
}

}  // namespace bourse::exec
