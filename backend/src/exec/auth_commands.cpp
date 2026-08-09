#include <memory>
#include <string>
#include <vector>

#include "bourse/auth/auth_service.hpp"
#include "bourse/core/clock.hpp"
#include "bourse/exec/command.hpp"

namespace bourse::exec {
namespace {

std::string toUpperAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
  }
  return out;
}

/// `AUTH password` or `AUTH username password`.
///
/// The one-argument form is Redis's legacy shape and resolves to the `default`
/// user, so a client written against Redis works unchanged. Marked no-auth for
/// the obvious reason: it is how a connection stops being anonymous.
class AuthCommand final : public Command {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "AUTH"; }

  [[nodiscard]] int arity() const noexcept override { return -2; }

  [[nodiscard]] bool isNoAuth() const noexcept override { return true; }

  [[nodiscard]] std::string_view summary() const noexcept override { return "AUTH [username] password"; }

  Reply execute(CommandContext& context, const std::vector<std::string>& argv) override {
    if (argv.size() > 3) {
      return Reply::error("ERR wrong number of arguments for 'AUTH' command");
    }
    if (context.server.auth == nullptr || !context.server.auth->enabled()) {
      // Matching Redis's wording exactly: clients special-case this string to
      // mean "the server has no password", rather than treating it as a
      // credential failure and giving up.
      return Reply::error(
          "ERR Client sent AUTH, but no password is set. Did you mean AUTH <username> <password>?");
    }

    const std::string username = argv.size() == 3 ? argv[1] : "default";
    const std::string& password = argv.size() == 3 ? argv[2] : argv[1];

    Result<auth::Principal> principal =
        context.server.auth->verifyCredentials(username, password, context.client_id);
    if (!principal.ok()) {
      return Reply::error("WRONGPASS " + principal.status().message());
    }

    // Writing the principal into the context is what promotes the connection:
    // the RESP loop copies it back onto the Connection after dispatch.
    context.principal = std::move(principal).value();
    return Reply::ok();
  }
};

/// `WHOAMI` -- the current identity on this connection.
class WhoAmICommand final : public Command {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "WHOAMI"; }

  [[nodiscard]] int arity() const noexcept override { return 1; }

  [[nodiscard]] bool isNoAuth() const noexcept override { return true; }

  [[nodiscard]] std::string_view summary() const noexcept override { return "WHOAMI"; }

  Reply execute(CommandContext& context, const std::vector<std::string>& /*argv*/) override {
    std::vector<Reply> out;
    out.push_back(Reply::bulkString("username"));
    out.push_back(
        Reply::bulkString(context.principal.username.empty() ? "(anonymous)" : context.principal.username));
    out.push_back(Reply::bulkString("role"));
    out.push_back(Reply::bulkString(std::string(auth::toString(context.principal.role))));
    out.push_back(Reply::bulkString("auth_enabled"));
    const bool enabled = context.server.auth != nullptr && context.server.auth->enabled();
    out.push_back(Reply::bulkString(enabled ? "yes" : "no"));
    return Reply::array(std::move(out));
  }
};

/// `USER` -- account administration. Admin-only via `isAdmin()`, which the
/// registry enforces centrally; there is no check inside these subcommands.
class UserCommand final : public Command {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "USER"; }

  [[nodiscard]] int arity() const noexcept override { return -2; }

  [[nodiscard]] bool isAdmin() const noexcept override { return true; }

  /// Not `isWrite()`: user records live outside the keyspace and are never
  /// journalled. Replaying `USER ADD alice hunter2` from the WAL would put a
  /// plaintext password in a file on disk, which is worse than not persisting
  /// users at all. See docs/security.md.
  [[nodiscard]] bool isWrite() const noexcept override { return false; }

  [[nodiscard]] std::string_view summary() const noexcept override {
    return "USER LIST | ADD username password role | PASSWD username password | "
           "ROLE username role | DEL username";
  }

  Reply execute(CommandContext& context, const std::vector<std::string>& argv) override {
    if (context.server.auth == nullptr) {
      return Reply::error("ERR authentication is not configured on this server");
    }
    auth::AuthService& service = *context.server.auth;
    const std::string subcommand = toUpperAscii(argv[1]);

    if (subcommand == "LIST") {
      std::vector<Reply> rows;
      for (const auth::UserRecord& user : service.listUsers()) {
        std::vector<Reply> row;
        row.push_back(Reply::bulkString("username"));
        row.push_back(Reply::bulkString(user.username));
        row.push_back(Reply::bulkString("role"));
        row.push_back(Reply::bulkString(std::string(auth::toString(user.role))));
        rows.push_back(Reply::array(std::move(row)));
      }
      return Reply::array(std::move(rows));
    }

    if (subcommand == "ADD") {
      if (argv.size() != 5) {
        return Reply::error("ERR USER ADD requires username, password and role");
      }
      auth::Role role{};
      if (!auth::parseRole(argv[4], role)) {
        return Reply::error("ERR role must be one of: viewer, trader, admin");
      }
      const Status added = service.addUser(argv[2], argv[3], role);
      return added.ok() ? Reply::ok() : Reply::error("ERR " + added.message());
    }

    if (subcommand == "PASSWD") {
      if (argv.size() != 4) {
        return Reply::error("ERR USER PASSWD requires username and password");
      }
      const Status changed = service.setPassword(argv[2], argv[3]);
      return changed.ok() ? Reply::ok() : Reply::error("ERR " + changed.message());
    }

    if (subcommand == "ROLE") {
      if (argv.size() != 4) {
        return Reply::error("ERR USER ROLE requires username and role");
      }
      auth::Role role{};
      if (!auth::parseRole(argv[3], role)) {
        return Reply::error("ERR role must be one of: viewer, trader, admin");
      }
      const Status changed = service.setRole(argv[2], role);
      return changed.ok() ? Reply::ok() : Reply::error("ERR " + changed.message());
    }

    if (subcommand == "DEL") {
      if (argv.size() != 3) {
        return Reply::error("ERR USER DEL requires a username");
      }
      // Refusing to delete the account you are using is not paternalism: it is
      // the difference between a mistake and a server nobody can administer.
      if (argv[2] == context.principal.username) {
        return Reply::error("ERR cannot delete the account you are authenticated as");
      }
      const Status removed = service.removeUser(argv[2]);
      return removed.ok() ? Reply::ok() : Reply::error("ERR " + removed.message());
    }

    return Reply::error("ERR unknown USER subcommand '" + argv[1] +
                        "'; expected LIST, ADD, PASSWD, ROLE or DEL");
  }
};

}  // namespace

void registerAuthCommands(CommandRegistry& registry) {
  registry.registerCommand(std::make_unique<AuthCommand>());
  registry.registerCommand(std::make_unique<WhoAmICommand>());
  registry.registerCommand(std::make_unique<UserCommand>());
}

}  // namespace bourse::exec
