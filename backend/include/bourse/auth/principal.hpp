#pragma once

#include <cstdint>
#include <string>
#include <string_view>

/// \file principal.hpp
/// Who is running a command.
///
/// Deliberately a leaf header with no dependencies beyond the standard
/// library: both `net` and `exec` include it, and neither should have to pull
/// in the whole auth service to ask "may this caller do this?".

namespace bourse::auth {

/// Ordered from least to most privileged, so `role >= required` is the whole
/// permission check. Making that a numeric comparison rather than a set of
/// booleans is what keeps the enforcement to one line in one place.
enum class Role : std::uint8_t {
  kAnonymous = 0,  ///< no credentials presented
  kViewer = 1,     ///< read-only: GET, BOOK, SELECT
  kTrader = 2,     ///< everything a viewer can do, plus writes and orders
  kAdmin = 3,      ///< plus FLUSHALL, CONFIG, user management
};

constexpr std::string_view toString(Role role) noexcept {
  switch (role) {
    case Role::kAnonymous: return "anonymous";
    case Role::kViewer: return "viewer";
    case Role::kTrader: return "trader";
    case Role::kAdmin: return "admin";
  }
  return "unknown";
}

/// Parses a role name. Returns false for anything unrecognised rather than
/// silently defaulting -- a typo in a config file must not quietly grant or
/// revoke privileges.
bool parseRole(std::string_view text, Role& out) noexcept;

/// An authenticated (or anonymous) identity, attached to a request or a
/// connection for as long as it is being served.
struct Principal {
  std::string username;  ///< empty when anonymous
  Role role = Role::kAnonymous;

  [[nodiscard]] bool authenticated() const noexcept { return role != Role::kAnonymous; }
  [[nodiscard]] bool can(Role required) const noexcept { return role >= required; }
};

}  // namespace bourse::auth
