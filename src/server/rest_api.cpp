#include "bourse/server/rest_api.hpp"

#include <string>
#include <vector>

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

/// Runs a command through the same registry the RESP codec uses and renders the
/// Reply as JSON. `connection` is null, which is why SUBSCRIBE refuses over
/// HTTP rather than half-working.
void dispatchAsJson(exec::ServerContext& context, const exec::CommandRegistry& registry,
                    const std::vector<std::string>& argv, net::HttpResponse& response) {
  if (argv.empty()) {
    response = net::HttpResponse::error(400, "empty command");
    return;
  }
  exec::CommandContext command_context{context, nullptr};
  const Reply reply = registry.dispatch(command_context, argv);
  response = net::HttpResponse::json(reply.toJson(), reply.isError() ? 400 : 200);
}

std::string jsonString(std::string_view text) {
  std::string out;
  exec::appendJsonEscaped(out, text);
  return out;
}

}  // namespace

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
    dispatchAsJson(context, registry, {"KEYS", request.queryParam("pattern", "*")}, response);
  });

  router.get("/api/keys/:key", [&context, &registry](const net::HttpRequest& request, net::HttpResponse& response) {
    dispatchAsJson(context, registry, {"GET", request.pathParam("key")}, response);
  });

  router.put("/api/keys/:key", [&context, &registry](const net::HttpRequest& request, net::HttpResponse& response) {
    std::vector<std::string> argv = {"SET", request.pathParam("key"), request.body};
    const std::string ttl = request.queryParam("ex");
    if (!ttl.empty()) {
      argv.emplace_back("EX");
      argv.push_back(ttl);
    }
    dispatchAsJson(context, registry, argv, response);
  });

  router.del("/api/keys/:key", [&context, &registry](const net::HttpRequest& request, net::HttpResponse& response) {
    dispatchAsJson(context, registry, {"DEL", request.pathParam("key")}, response);
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
    dispatchAsJson(context, registry, argv, response);
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
  router.get("/api/symbols", [&context, &registry](const net::HttpRequest&, net::HttpResponse& response) {
    dispatchAsJson(context, registry, {"SYMBOLS"}, response);
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
    dispatchAsJson(context, registry, argv, response);
  });

  router.del("/api/orders/:symbol/:id", [&context, &registry](const net::HttpRequest& request,
                                                              net::HttpResponse& response) {
    dispatchAsJson(context, registry, {"CANCEL", request.pathParam("symbol"), request.pathParam("id")},
                   response);
  });
}

}  // namespace bourse::server
