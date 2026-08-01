#pragma once

#include "bourse/exec/command.hpp"
#include "bourse/net/router.hpp"

namespace bourse::server {

/// Installs every HTTP route on `router`.
///
/// The REST layer deliberately owns no logic. Each route translates an HTTP
/// request into the same `CommandRegistry::dispatch` call the RESP codec makes,
/// then renders the resulting `Reply` as JSON. That is the payoff of commands
/// returning objects rather than bytes: the two transports cannot drift apart,
/// because there is only one implementation behind both.
void buildRestApi(net::Router& router, exec::ServerContext& context, const exec::CommandRegistry& registry);

/// Installs `/api/auth/*`: login, logout, current identity, user administration.
///
/// Separate from `buildRestApi` because these are the only routes that do not
/// go through the command registry -- they manage credentials rather than
/// server state -- and keeping them in one function keeps the set that needs
/// its own permission check small enough to audit at a glance.
void buildAuthApi(net::Router& router, exec::ServerContext& context);

/// Resolves a bearer token into `HttpRequest::principal`, and refuses
/// unauthenticated requests to non-public paths when auth is enabled.
///
/// Identification only; the command registry makes the finer-grained write and
/// admin decisions, so there is exactly one copy of that policy shared by RESP
/// and REST. Must be registered *after* the CORS middleware, so a rejected
/// pre-flight still carries the headers a browser needs to read the 401.
[[nodiscard]] net::Middleware makeRestAuthMiddleware(exec::ServerContext& context);

}  // namespace bourse::server
