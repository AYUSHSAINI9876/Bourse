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

}  // namespace bourse::server
