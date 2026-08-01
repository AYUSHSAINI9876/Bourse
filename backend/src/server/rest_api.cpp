#include "bourse/server/rest_api.hpp"

#include <string>
#include <vector>

#include "bourse/auth/auth_service.hpp"
#include "bourse/auth/crypto.hpp"
#include "bourse/cache/keyspace.hpp"
#include "bourse/core/clock.hpp"
#include "bourse/core/metrics.hpp"
#include "bourse/match/matching_engine.hpp"
#include "bourse/net/resp_codec.hpp"
#include "bourse/sql/engine.hpp"

#include "bourse/dashboard_asset.hpp"

namespace bourse::server {
namespace {

using exec::Reply;

/// Identifies the caller for login rate limiting.
///
/// `X-Forwarded-For` is trusted here because the only deployment shape this
/// server supports puts it behind exactly one reverse proxy (Render, Fly,
/// Cloud Run) that overwrites the header. Exposed directly to the internet
/// the header is client-controlled and worthless -- which is why it is used
/// solely for throttling and never for an authorization decision.
std::string clientIdOf(const net::HttpRequest& request) {
  const std::string forwarded = request.header("X-Forwarded-For");
  if (!forwarded.empty()) {
    // Left-most entry is the original client; the rest are proxy hops.
    const std::size_t comma = forwarded.find(',');
    return comma == std::string::npos ? forwarded : forwarded.substr(0, comma);
  }
  return request.header("X-Real-IP", "unknown");
}

/// Extracts a bearer token. Returns empty when the header is absent or is not
/// a bearer credential -- `Basic` is not silently accepted.
std::string bearerTokenOf(const net::HttpRequest& request) {
  const std::string header = request.header("Authorization");
  constexpr std::string_view kPrefix = "Bearer ";
  if (header.size() <= kPrefix.size()) {
    return {};
  }
  // The scheme is case-insensitive per RFC 7235; the token is not.
  for (std::size_t i = 0; i < kPrefix.size(); ++i) {
    const char lhs = static_cast<char>(std::tolower(static_cast<unsigned char>(header[i])));
    const char rhs = static_cast<char>(std::tolower(static_cast<unsigned char>(kPrefix[i])));
    if (lhs != rhs) {
      return {};
    }
  }
  std::string token = header.substr(kPrefix.size());
  while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
    token.erase(token.begin());
  }
  return token;
}

/// Paths reachable without credentials even when auth is enforced.
///
/// An allowlist, so a route added tomorrow is protected by default. The
/// alternative -- listing what to protect -- fails open, and the failure is
/// invisible until someone finds the endpoint.
bool isPublicPath(const std::string& path) {
  // `/api/auth/me` is public deliberately. It is how the dashboard discovers
  // whether this server even requires credentials, and answering 401 would
  // make "auth is on and you are not signed in" indistinguishable from "this
  // build has no auth routes". It reveals only whether auth is enabled, which
  // any other endpoint already reveals by returning 401.
  return path == "/" || path == "/health" || path == "/api/auth/login" || path == "/api/auth/me";
}

/// Runs a command through the same registry the RESP codec uses and renders the
/// Reply as JSON. `connection` is null, which is why SUBSCRIBE refuses over
/// HTTP rather than half-working.
///
/// The request's principal is carried in, so the registry applies exactly the
/// same role check to `POST /api/command` that it applies to a RESP client.
/// Two protocols, one authorization decision.
void dispatchAsJson(exec::ServerContext& context, const exec::CommandRegistry& registry,
                    const net::HttpRequest& request, const std::vector<std::string>& argv,
                    net::HttpResponse& response) {
  if (argv.empty()) {
    response = net::HttpResponse::error(400, "empty command");
    return;
  }
  exec::CommandContext command_context{context, nullptr, request.principal, clientIdOf(request)};
  const Reply reply = registry.dispatch(command_context, argv);

  // A permission failure is a 403, not a 400. Returning 400 for everything
  // makes a client retry a request that will never succeed, and hides the one
  // error a user can actually act on.
  int status = 200;
  if (reply.isError()) {
    const std::string_view message = reply.text();
    if (message.rfind("NOAUTH", 0) == 0) {
      status = 401;
    } else if (message.rfind("NOPERM", 0) == 0 || message.rfind("WRONGPASS", 0) == 0) {
      status = 403;
    } else {
      status = 400;
    }
  }
  response = net::HttpResponse::json(reply.toJson(), status);
}

std::string jsonString(std::string_view text) {
  std::string out;
  exec::appendJsonEscaped(out, text);
  return out;
}

std::string roleJson(auth::Role role) {
  return jsonString(auth::toString(role));
}

}  // namespace

net::Middleware makeRestAuthMiddleware(exec::ServerContext& context) {
  return [&context](net::HttpRequest& request, net::HttpResponse& response,
                    const std::function<void()>& next) {
    auth::AuthService* service = context.auth;

    // Resolve a token whenever one is presented, even with enforcement off.
    // It costs one hash lookup and it means `/api/auth/me` and the dashboard's
    // identity display behave identically in both modes.
    if (service != nullptr) {
      const std::string token = bearerTokenOf(request);
      if (!token.empty()) {
        Result<auth::Principal> principal = service->authenticate(token);
        if (principal.ok()) {
          request.principal = std::move(principal).value();
        }
      }
    }

    if (service == nullptr || !service->enabled() || isPublicPath(request.path)) {
      next();
      return;
    }

    if (!request.principal.authenticated()) {
      response = net::HttpResponse::json(
          R"({"error":"authentication required","code":"NOAUTH"})", 401);
      // RFC 7235 requires a challenge on a 401. Browsers use it to decide
      // whether to prompt; omitting it makes fetch() failures harder to
      // diagnose than they need to be.
      response.setHeader("WWW-Authenticate", "Bearer realm=\"bourse\"");
      return;
    }

    // Coarse gate only. Finer-grained write and admin checks happen in the
    // command registry, which both protocols share -- doing them here as well
    // would be a second copy of the policy, free to drift from the first.
    next();
  };
}

void buildAuthApi(net::Router& router, exec::ServerContext& context) {
  const auto requireAuthService = [&context](net::HttpResponse& response) -> auth::AuthService* {
    if (context.auth == nullptr) {
      response = net::HttpResponse::json(
          R"({"error":"authentication is not configured on this server"})", 501);
      return nullptr;
    }
    return context.auth;
  };

  // ---- login ------------------------------------------------------------
  router.post("/api/auth/login", [&context, requireAuthService](const net::HttpRequest& request,
                                                                net::HttpResponse& response) {
    auth::AuthService* service = requireAuthService(response);
    if (service == nullptr) {
      return;
    }

    // Credentials arrive in the body, never the query string: query strings
    // land in proxy logs, browser history and Referer headers.
    const std::string username = net::jsonFieldOf(request.body, "username");
    const std::string password = net::jsonFieldOf(request.body, "password");
    if (username.empty() || password.empty()) {
      response = net::HttpResponse::json(
          R"({"error":"username and password are required"})", 400);
      return;
    }

    Result<auth::LoginResult> login = service->login(username, password, clientIdOf(request));
    if (!login.ok()) {
      // 429 for the throttle, 401 for bad credentials. A client that cannot
      // tell them apart retries into a longer lockout.
      const bool throttled = login.status().code() == ErrorCode::kUnsupported;
      response = net::HttpResponse::json(
          "{\"error\":" + jsonString(login.status().message()) + "}", throttled ? 429 : 401);
      if (!throttled) {
        response.setHeader("WWW-Authenticate", "Bearer realm=\"bourse\"");
      }
      return;
    }

    std::string payload = "{\"token\":" + jsonString(login.value().token);
    payload += ",\"username\":" + jsonString(login.value().username);
    payload += ",\"role\":" + roleJson(login.value().role);
    payload += ",\"expires_at_ms\":" + std::to_string(login.value().expires_at_ms);
    payload += "}";
    response = net::HttpResponse::json(std::move(payload));
    // The token must not be cached anywhere -- not by the browser, not by a
    // proxy, not by the CDN in front of the dashboard.
    response.setHeader("Cache-Control", "no-store");
  });

  // ---- logout -----------------------------------------------------------
  router.post("/api/auth/logout", [&context](const net::HttpRequest& request,
                                             net::HttpResponse& response) {
    if (context.auth == nullptr) {
      response = net::HttpResponse::json(R"({"ok":true})");
      return;
    }
    const std::string token = bearerTokenOf(request);
    const bool removed = !token.empty() && context.auth->logout(token);
    // Deliberately 200 either way. Reporting "that token did not exist" turns
    // logout into a token-validity oracle for an unauthenticated caller.
    response = net::HttpResponse::json(std::string("{\"ok\":true,\"revoked\":") +
                                       (removed ? "true" : "false") + "}");
  });

  // ---- current identity -------------------------------------------------
  router.get("/api/auth/me", [&context](const net::HttpRequest& request, net::HttpResponse& response) {
    const bool enabled = context.auth != nullptr && context.auth->enabled();
    std::string payload = "{\"authenticated\":";
    payload += request.principal.authenticated() ? "true" : "false";
    payload += ",\"auth_enabled\":";
    payload += enabled ? "true" : "false";
    payload += ",\"username\":" + jsonString(request.principal.username);
    payload += ",\"role\":" + roleJson(request.principal.role);
    payload += "}";
    response = net::HttpResponse::json(std::move(payload));
    response.setHeader("Cache-Control", "no-store");
  });

  // ---- user administration ---------------------------------------------
  //
  // These check the role inline because they do not pass through the command
  // registry. Every such route is listed here in one block, so the set that
  // needs its own check is small enough to audit by eye.
  const auto requireAdmin = [](const net::HttpRequest& request, net::HttpResponse& response) {
    if (request.principal.can(auth::Role::kAdmin)) {
      return true;
    }
    response = net::HttpResponse::json(R"({"error":"admin role required","code":"NOPERM"})", 403);
    return false;
  };

  router.get("/api/auth/users", [&context, requireAuthService, requireAdmin](
                                    const net::HttpRequest& request, net::HttpResponse& response) {
    auth::AuthService* service = requireAuthService(response);
    if (service == nullptr || !requireAdmin(request, response)) {
      return;
    }
    std::string payload = "{\"users\":[";
    bool first = true;
    for (const auth::UserRecord& user : service->listUsers()) {
      if (!first) {
        payload += ',';
      }
      first = false;
      payload += "{\"username\":" + jsonString(user.username);
      payload += ",\"role\":" + roleJson(user.role);
      payload += ",\"created_at_ms\":" + std::to_string(user.created_at_ms) + "}";
    }
    payload += "]}";
    response = net::HttpResponse::json(std::move(payload));
  });

  router.post("/api/auth/users", [&context, requireAuthService, requireAdmin](
                                     const net::HttpRequest& request, net::HttpResponse& response) {
    auth::AuthService* service = requireAuthService(response);
    if (service == nullptr || !requireAdmin(request, response)) {
      return;
    }
    const std::string username = net::jsonFieldOf(request.body, "username");
    const std::string password = net::jsonFieldOf(request.body, "password");
    const std::string role_text = net::jsonFieldOf(request.body, "role");
    auth::Role role{};
    if (!auth::parseRole(role_text, role)) {
      response = net::HttpResponse::json(
          R"({"error":"role must be one of: viewer, trader, admin"})", 400);
      return;
    }
    const Status added = service->addUser(username, password, role);
    if (!added.ok()) {
      const int status = added.code() == ErrorCode::kAlreadyExists ? 409 : 400;
      response = net::HttpResponse::json("{\"error\":" + jsonString(added.message()) + "}", status);
      return;
    }
    response = net::HttpResponse::json(R"({"ok":true})", 201);
  });

  router.del("/api/auth/users/:username", [&context, requireAuthService, requireAdmin](
                                               const net::HttpRequest& request,
                                               net::HttpResponse& response) {
    auth::AuthService* service = requireAuthService(response);
    if (service == nullptr || !requireAdmin(request, response)) {
      return;
    }
    const std::string username = request.pathParam("username");
    if (username == request.principal.username) {
      response = net::HttpResponse::json(
          R"({"error":"cannot delete the account you are authenticated as"})", 400);
      return;
    }
    const Status removed = service->removeUser(username);
    if (!removed.ok()) {
      response = net::HttpResponse::json("{\"error\":" + jsonString(removed.message()) + "}", 404);
      return;
    }
    response = net::HttpResponse::json(R"({"ok":true})");
  });
}

void buildRestApi(net::Router& router, exec::ServerContext& context, const exec::CommandRegistry& registry) {
  // ---- dashboard --------------------------------------------------------
  router.get("/", [](const net::HttpRequest&, net::HttpResponse& response) {
    // Baked into the binary at build time, so the server is a single
    // self-contained artefact with no asset path to get wrong on deploy.
    response = net::HttpResponse::html(std::string(assets::kDashboardHtml));
    response.setHeader("Cache-Control", "no-cache");
  });

  // ---- operational ------------------------------------------------------
  router.get("/health", [&context](const net::HttpRequest&, net::HttpResponse& response) {
    const std::int64_t uptime_ms = nowMillis() - context.started_at_ms;
    response = net::HttpResponse::json("{\"status\":\"ok\",\"uptime_ms\":" + std::to_string(uptime_ms) +
                                       ",\"version\":" + jsonString(context.version) + "}");
  });

  router.get("/metrics", [](const net::HttpRequest&, net::HttpResponse& response) {
    response = net::HttpResponse::text(MetricsRegistry::instance().renderPrometheus());
    response.setHeader("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
  });

  router.get("/api/stats", [&context](const net::HttpRequest&, net::HttpResponse& response) {
    const cache::KeyspaceStats keyspace = context.keyspace->stats();
    std::string payload = "{";
    payload.append("\"uptime_ms\":").append(std::to_string(nowMillis() - context.started_at_ms));
    payload.append(",\"keys\":").append(std::to_string(keyspace.keys));
    payload.append(",\"memory_bytes\":").append(std::to_string(keyspace.memory_bytes));
    payload.append(",\"hits\":").append(std::to_string(keyspace.hits));
    payload.append(",\"misses\":").append(std::to_string(keyspace.misses));
    payload.append(",\"expired\":").append(std::to_string(keyspace.expired));
    payload.append(",\"evicted\":").append(std::to_string(keyspace.evicted));
    payload.append(",\"eviction_policy\":").append(jsonString(context.keyspace->evictionPolicyName()));

    if (context.matching_engine != nullptr) {
      const match::EngineStats engine = context.matching_engine->stats();
      payload.append(",\"orders_accepted\":").append(std::to_string(engine.orders_accepted));
      payload.append(",\"orders_rejected\":").append(std::to_string(engine.orders_rejected));
      payload.append(",\"orders_cancelled\":").append(std::to_string(engine.orders_cancelled));
      payload.append(",\"trades_executed\":").append(std::to_string(engine.trades_executed));
      payload.append(",\"volume_traded\":").append(std::to_string(engine.volume_traded));
      payload.append(",\"resting_orders\":").append(std::to_string(engine.resting_orders));
    }

    const Histogram::Snapshot command = MetricsRegistry::instance().histogram("bourse_command_latency_nanos").snapshot();
    payload.append(",\"command_latency\":{\"count\":").append(std::to_string(command.count));
    payload.append(",\"p50\":").append(std::to_string(command.p50));
    payload.append(",\"p90\":").append(std::to_string(command.p90));
    payload.append(",\"p99\":").append(std::to_string(command.p99));
    payload.append(",\"p999\":").append(std::to_string(command.p999));
    payload.append(",\"max\":").append(std::to_string(command.max)).append("}");

    const Histogram::Snapshot matching = MetricsRegistry::instance().histogram("bourse_match_latency_nanos").snapshot();
    payload.append(",\"match_latency\":{\"count\":").append(std::to_string(matching.count));
    payload.append(",\"p50\":").append(std::to_string(matching.p50));
    payload.append(",\"p99\":").append(std::to_string(matching.p99)).append("}");

    payload.append(",\"connections\":")
        .append(std::to_string(MetricsRegistry::instance().gauge("bourse_connections_active").value()));
    payload.append(",\"commands_processed\":")
        .append(std::to_string(MetricsRegistry::instance().counter("bourse_commands_processed_total").value()));
    payload.push_back('}');

    response = net::HttpResponse::json(std::move(payload));
  });

  // ---- keyspace ---------------------------------------------------------
  router.get("/api/keys", [&context, &registry](const net::HttpRequest& request, net::HttpResponse& response) {
    dispatchAsJson(context, registry, request, {"KEYS", request.queryParam("pattern", "*")}, response);
  });

  router.get("/api/keys/:key", [&context, &registry](const net::HttpRequest& request, net::HttpResponse& response) {
    dispatchAsJson(context, registry, request, {"GET", request.pathParam("key")}, response);
  });

  router.put("/api/keys/:key", [&context, &registry](const net::HttpRequest& request, net::HttpResponse& response) {
    std::vector<std::string> argv = {"SET", request.pathParam("key"), request.body};
    const std::string ttl = request.queryParam("ex");
    if (!ttl.empty()) {
      argv.emplace_back("EX");
      argv.push_back(ttl);
    }
    dispatchAsJson(context, registry, request, argv, response);
  });

  router.del("/api/keys/:key", [&context, &registry](const net::HttpRequest& request, net::HttpResponse& response) {
    dispatchAsJson(context, registry, request, {"DEL", request.pathParam("key")}, response);
  });

  // ---- generic command passthrough --------------------------------------
  router.post("/api/command", [&context, &registry](const net::HttpRequest& request, net::HttpResponse& response) {
    bool ok = false;
    // Reuses the RESP inline splitter so quoting behaves identically over
    // both transports.
    const std::vector<std::string> argv = net::splitInlineCommand(request.body, &ok);
    if (!ok) {
      response = net::HttpResponse::error(400, "unbalanced quotes in command");
      return;
    }
    dispatchAsJson(context, registry, request, argv, response);
  });

  // ---- SQL --------------------------------------------------------------
  router.post("/api/sql", [&context](const net::HttpRequest& request, net::HttpResponse& response) {
    if (context.sql_engine == nullptr) {
      response = net::HttpResponse::error(503, "SQL engine is not enabled");
      return;
    }
    Result<sql::ResultSet> result = context.sql_engine->execute(request.body);
    if (!result.ok()) {
      response = net::HttpResponse::error(400, result.status().message());
      return;
    }
    response = net::HttpResponse::json(result.value().toJson());
  });

  router.get("/api/tables", [&context](const net::HttpRequest&, net::HttpResponse& response) {
    if (context.sql_engine == nullptr) {
      response = net::HttpResponse::error(503, "SQL engine is not enabled");
      return;
    }
    std::string payload = "[";
    const std::vector<std::string> names = context.sql_engine->tableNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (i != 0) {
        payload.push_back(',');
      }
      payload.append(jsonString(names[i]));
    }
    payload.push_back(']');
    response = net::HttpResponse::json(std::move(payload));
  });

  // ---- exchange ---------------------------------------------------------
  router.get("/api/symbols", [&context, &registry](const net::HttpRequest& request, net::HttpResponse& response) {
    dispatchAsJson(context, registry, request, {"SYMBOLS"}, response);
  });

  router.get("/api/book/:symbol", [&context](const net::HttpRequest& request, net::HttpResponse& response) {
    if (context.matching_engine == nullptr) {
      response = net::HttpResponse::error(503, "matching engine is not enabled");
      return;
    }
    const std::string symbol = request.pathParam("symbol");
    const std::optional<match::BookSnapshot> snapshot = context.matching_engine->snapshot(symbol, 15);
    if (!snapshot.has_value()) {
      response = net::HttpResponse::json("{\"symbol\":" + jsonString(symbol) + ",\"bids\":[],\"asks\":[]}");
      return;
    }

    auto side = [](const std::vector<match::DepthLevel>& levels) {
      std::string out = "[";
      for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        out.append("{\"price\":").append(jsonString(match::formatPrice(levels[i].price)));
        out.append(",\"quantity\":").append(std::to_string(levels[i].quantity));
        out.append(",\"orders\":").append(std::to_string(levels[i].orders)).push_back('}');
      }
      out.push_back(']');
      return out;
    };

    std::string payload = "{\"symbol\":" + jsonString(snapshot->symbol);
    payload.append(",\"bids\":").append(side(snapshot->bids));
    payload.append(",\"asks\":").append(side(snapshot->asks));
    const std::optional<match::Price> last = context.matching_engine->lastTradePrice(symbol);
    payload.append(",\"last\":")
        .append(last.has_value() ? jsonString(match::formatPrice(*last)) : "null");
    payload.push_back('}');
    response = net::HttpResponse::json(std::move(payload));
  });

  router.get("/api/trades/:symbol", [&context](const net::HttpRequest& request, net::HttpResponse& response) {
    if (context.matching_engine == nullptr) {
      response = net::HttpResponse::error(503, "matching engine is not enabled");
      return;
    }
    const std::vector<match::Trade> trades =
        context.matching_engine->recentTrades(request.pathParam("symbol"), 40);

    std::string payload = "[";
    for (std::size_t i = 0; i < trades.size(); ++i) {
      if (i != 0) {
        payload.push_back(',');
      }
      payload.append("{\"sequence\":").append(std::to_string(trades[i].sequence));
      payload.append(",\"price\":").append(jsonString(match::formatPrice(trades[i].price)));
      payload.append(",\"quantity\":").append(std::to_string(trades[i].quantity));
      payload.append(",\"side\":").append(jsonString(match::toString(trades[i].aggressor_side)));
      payload.push_back('}');
    }
    payload.push_back(']');
    response = net::HttpResponse::json(std::move(payload));
  });

  router.post("/api/orders", [&context, &registry](const net::HttpRequest& request, net::HttpResponse& response) {
    std::vector<std::string> argv = {"ORDER", request.queryParam("symbol"), request.queryParam("side"),
                                     request.queryParam("type", "LIMIT"), request.queryParam("quantity")};
    const std::string price = request.queryParam("price");
    if (!price.empty()) {
      argv.push_back(price);
    }
    const std::string tif = request.queryParam("tif");
    if (!tif.empty()) {
      argv.push_back(tif);
    }
    dispatchAsJson(context, registry, request, argv, response);
  });

  router.del("/api/orders/:symbol/:id", [&context, &registry](const net::HttpRequest& request,
                                                              net::HttpResponse& response) {
    dispatchAsJson(context, registry, request, {"CANCEL", request.pathParam("symbol"), request.pathParam("id")},
                   response);
  });
}

}  // namespace bourse::server
