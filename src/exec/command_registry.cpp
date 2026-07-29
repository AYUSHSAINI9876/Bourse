#include <algorithm>
#include <cctype>

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

  return const_cast<Command*>(command)->execute(context, argv);
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
  registerAdminCommands(*registry);
  BOURSE_LOG_INFO("command registry initialised with ", registry->size(), " verbs");
  return registry;
}

}  // namespace bourse::exec
