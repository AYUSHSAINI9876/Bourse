#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "bourse/cache/value.hpp"
#include "bourse/exec/command.hpp"
#include "bourse/match/matching_engine.hpp"

namespace bourse::exec {
namespace {

using match::BookSnapshot;
using match::ExecutionReport;
using match::MatchingEngine;
using match::Order;
using match::OrderType;
using match::Price;
using match::Quantity;
using match::Side;
using match::TimeInForce;
using match::Trade;

Reply noEngine() { return Reply::error("ERR matching engine is not enabled on this server"); }

std::string normaliseSymbol(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (char c : raw) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

Reply encodeTrade(const Trade& trade) {
  return Reply::array({
      Reply::bulkString("sequence"), Reply::integer(static_cast<std::int64_t>(trade.sequence)),
      Reply::bulkString("price"), Reply::bulkString(match::formatPrice(trade.price)),
      Reply::bulkString("quantity"), Reply::integer(trade.quantity),
      Reply::bulkString("aggressor"), Reply::bulkString(match::toString(trade.aggressor_side)),
      Reply::bulkString("resting_order"), Reply::integer(static_cast<std::int64_t>(trade.resting_order_id)),
      Reply::bulkString("aggressing_order"),
      Reply::integer(static_cast<std::int64_t>(trade.aggressing_order_id)),
  });
}

Reply encodeReport(const ExecutionReport& report) {
  std::vector<Reply> trades;
  trades.reserve(report.trades.size());
  for (const Trade& trade : report.trades) {
    trades.push_back(encodeTrade(trade));
  }

  std::vector<Reply> fields = {
      Reply::bulkString("order_id"), Reply::integer(static_cast<std::int64_t>(report.order_id)),
      Reply::bulkString("status"),   Reply::bulkString(match::toString(report.status)),
      Reply::bulkString("filled"),   Reply::integer(report.filled),
      Reply::bulkString("remaining"), Reply::integer(report.remaining),
      Reply::bulkString("avg_price"), Reply::bulkString(match::formatPrice(report.average_price)),
      Reply::bulkString("trades"),   Reply::array(std::move(trades)),
  };
  if (!report.reject_reason.empty()) {
    fields.push_back(Reply::bulkString("reason"));
    fields.push_back(Reply::bulkString(report.reject_reason));
  }
  return Reply::array(std::move(fields));
}

Reply encodeSnapshot(const BookSnapshot& snapshot) {
  auto side = [](const std::vector<match::DepthLevel>& levels) {
    std::vector<Reply> out;
    out.reserve(levels.size());
    for (const match::DepthLevel& level : levels) {
      out.push_back(Reply::array({Reply::bulkString(match::formatPrice(level.price)),
                                  Reply::integer(level.quantity),
                                  Reply::integer(static_cast<std::int64_t>(level.orders))}));
    }
    return Reply::array(std::move(out));
  };

  return Reply::array({
      Reply::bulkString("symbol"), Reply::bulkString(snapshot.symbol),
      Reply::bulkString("bids"),   side(snapshot.bids),
      Reply::bulkString("asks"),   side(snapshot.asks),
  });
}

void add(CommandRegistry& registry, std::string name, int arity, bool is_write, std::string summary,
         LambdaCommand::Handler handler) {
  registry.registerCommand(std::make_unique<LambdaCommand>(std::move(name), arity, is_write,
                                                           std::move(summary), std::move(handler)));
}

/// `ORDER symbol side type quantity [price] [tif]`
///
/// An explicit class rather than a lambda: the argument grammar is positional
/// with an optional price whose presence depends on the order type, and that
/// is exactly the kind of parsing that earns a name and its own test.
class OrderCommand final : public Command {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "ORDER"; }
  [[nodiscard]] int arity() const noexcept override { return -5; }
  [[nodiscard]] bool isWrite() const noexcept override { return true; }
  [[nodiscard]] std::string_view summary() const noexcept override {
    return "ORDER symbol BUY|SELL LIMIT|MARKET quantity [price] [GTC|IOC|FOK]";
  }

  Reply execute(CommandContext& context, const std::vector<std::string>& argv) override {
    MatchingEngine* engine = context.server.matching_engine;
    if (engine == nullptr) {
      return noEngine();
    }

    Side side{};
    if (!match::parseSide(argv[2], side)) {
      return Reply::error("ERR side must be BUY or SELL");
    }
    OrderType type{};
    if (!match::parseOrderType(argv[3], type)) {
      return Reply::error("ERR type must be LIMIT or MARKET");
    }

    Result<std::int64_t> quantity = cache::parseInteger(argv[4]);
    if (!quantity.ok() || quantity.value() <= 0) {
      return Reply::error("ERR quantity must be a positive integer");
    }

    Price price = 0;
    std::size_t cursor = 5;
    if (type == OrderType::kLimit) {
      if (argv.size() <= cursor) {
        return Reply::error("ERR LIMIT orders require a price");
      }
      if (!match::parsePrice(argv[cursor], price) || price <= 0) {
        return Reply::error("ERR price must be a positive decimal");
      }
      ++cursor;
    }

    TimeInForce tif = TimeInForce::kGoodTilCancel;
    if (argv.size() > cursor && !match::parseTimeInForce(argv[cursor], tif)) {
      return Reply::error("ERR time-in-force must be GTC, IOC or FOK");
    }
    if (type == OrderType::kMarket && tif == TimeInForce::kGoodTilCancel) {
      // A market order can never rest, so GTC is meaningless on one. Treating
      // it as IOC matches every venue's behaviour and avoids a silent surprise.
      tif = TimeInForce::kImmediateOrCancel;
    }

    Order order;
    order.id = engine->nextOrderId();
    order.side = side;
    order.type = type;
    order.time_in_force = tif;
    order.price = price;
    order.quantity = quantity.value();

    return encodeReport(engine->submit(normaliseSymbol(argv[1]), order));
  }
};

}  // namespace

void registerExchangeCommands(CommandRegistry& registry) {
  registry.registerCommand(std::make_unique<OrderCommand>());

  add(registry, "CANCEL", 3, true, "CANCEL symbol order-id",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        MatchingEngine* engine = ctx.server.matching_engine;
        if (engine == nullptr) {
          return noEngine();
        }
        Result<std::int64_t> id = cache::parseInteger(argv[2]);
        if (!id.ok() || id.value() <= 0) {
          return Reply::error("ERR order id must be a positive integer");
        }
        ExecutionReport report;
        const bool cancelled =
            engine->cancel(normaliseSymbol(argv[1]), static_cast<match::OrderId>(id.value()), &report);
        if (!cancelled) {
          return Reply::integer(0);
        }
        return encodeReport(report);
      });

  add(registry, "AMEND", 5, true, "AMEND symbol order-id price quantity",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        MatchingEngine* engine = ctx.server.matching_engine;
        if (engine == nullptr) {
          return noEngine();
        }
        Result<std::int64_t> id = cache::parseInteger(argv[2]);
        if (!id.ok() || id.value() <= 0) {
          return Reply::error("ERR order id must be a positive integer");
        }
        Price price = 0;
        if (!match::parsePrice(argv[3], price) || price <= 0) {
          return Reply::error("ERR price must be a positive decimal");
        }
        Result<std::int64_t> quantity = cache::parseInteger(argv[4]);
        if (!quantity.ok() || quantity.value() <= 0) {
          return Reply::error("ERR quantity must be a positive integer");
        }
        bool found = false;
        const ExecutionReport report = engine->amend(
            normaliseSymbol(argv[1]), static_cast<match::OrderId>(id.value()), price, quantity.value(), &found);
        if (!found) {
          return Reply::integer(0);
        }
        return encodeReport(report);
      });

  add(registry, "BOOK", -2, false, "BOOK symbol [depth]",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        MatchingEngine* engine = ctx.server.matching_engine;
        if (engine == nullptr) {
          return noEngine();
        }
        std::size_t depth = 10;
        if (argv.size() >= 3) {
          Result<std::int64_t> requested = cache::parseInteger(argv[2]);
          if (!requested.ok() || requested.value() <= 0) {
            return Reply::error("ERR depth must be a positive integer");
          }
          depth = static_cast<std::size_t>(std::min<std::int64_t>(requested.value(), 500));
        }
        std::optional<BookSnapshot> snapshot = engine->snapshot(normaliseSymbol(argv[1]), depth);
        if (!snapshot.has_value()) {
          return Reply::nullArray();
        }
        return encodeSnapshot(*snapshot);
      });

  add(registry, "TRADES", -2, false, "TRADES symbol [limit]",
      [](CommandContext& ctx, const std::vector<std::string>& argv) {
        MatchingEngine* engine = ctx.server.matching_engine;
        if (engine == nullptr) {
          return noEngine();
        }
        std::size_t limit = 20;
        if (argv.size() >= 3) {
          Result<std::int64_t> requested = cache::parseInteger(argv[2]);
          if (!requested.ok() || requested.value() <= 0) {
            return Reply::error("ERR limit must be a positive integer");
          }
          limit = static_cast<std::size_t>(std::min<std::int64_t>(requested.value(), 1000));
        }
        std::vector<Reply> out;
        for (const Trade& trade : engine->recentTrades(normaliseSymbol(argv[1]), limit)) {
          out.push_back(encodeTrade(trade));
        }
        return Reply::array(std::move(out));
      });

  add(registry, "SYMBOLS", 1, false, "SYMBOLS",
      [](CommandContext& ctx, const std::vector<std::string>&) {
        MatchingEngine* engine = ctx.server.matching_engine;
        if (engine == nullptr) {
          return noEngine();
        }
        return Reply::stringArray(engine->symbols());
      });

  add(registry, "EXCHANGE", 1, false, "EXCHANGE",
      [](CommandContext& ctx, const std::vector<std::string>&) {
        MatchingEngine* engine = ctx.server.matching_engine;
        if (engine == nullptr) {
          return noEngine();
        }
        const match::EngineStats stats = engine->stats();
        return Reply::array({
            Reply::bulkString("orders_accepted"),
            Reply::integer(static_cast<std::int64_t>(stats.orders_accepted)),
            Reply::bulkString("orders_rejected"),
            Reply::integer(static_cast<std::int64_t>(stats.orders_rejected)),
            Reply::bulkString("orders_cancelled"),
            Reply::integer(static_cast<std::int64_t>(stats.orders_cancelled)),
            Reply::bulkString("trades_executed"),
            Reply::integer(static_cast<std::int64_t>(stats.trades_executed)),
            Reply::bulkString("volume_traded"),
            Reply::integer(static_cast<std::int64_t>(stats.volume_traded)),
            Reply::bulkString("symbols"), Reply::integer(static_cast<std::int64_t>(stats.symbols)),
            Reply::bulkString("resting_orders"),
            Reply::integer(static_cast<std::int64_t>(stats.resting_orders)),
        });
      });
}

}  // namespace bourse::exec
