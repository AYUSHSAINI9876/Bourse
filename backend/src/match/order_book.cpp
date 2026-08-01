#include "bourse/match/order_book.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <utility>

#include "bourse/core/clock.hpp"

namespace bourse::match {
namespace {

std::string upper(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// enum and price helpers
// ---------------------------------------------------------------------------

const char* toString(Side side) noexcept { return side == Side::kBuy ? "BUY" : "SELL"; }

const char* toString(OrderType type) noexcept { return type == OrderType::kLimit ? "LIMIT" : "MARKET"; }

const char* toString(TimeInForce tif) noexcept {
  switch (tif) {
    case TimeInForce::kGoodTilCancel: return "GTC";
    case TimeInForce::kImmediateOrCancel: return "IOC";
    case TimeInForce::kFillOrKill: return "FOK";
  }
  return "GTC";
}

const char* toString(OrderStatus status) noexcept {
  switch (status) {
    case OrderStatus::kNew: return "NEW";
    case OrderStatus::kPartiallyFilled: return "PARTIALLY_FILLED";
    case OrderStatus::kFilled: return "FILLED";
    case OrderStatus::kCancelled: return "CANCELLED";
    case OrderStatus::kRejected: return "REJECTED";
  }
  return "NEW";
}

bool parseSide(std::string_view text, Side& out) noexcept {
  const std::string value = upper(text);
  if (value == "BUY" || value == "B" || value == "BID") {
    out = Side::kBuy;
    return true;
  }
  if (value == "SELL" || value == "S" || value == "ASK") {
    out = Side::kSell;
    return true;
  }
  return false;
}

bool parseOrderType(std::string_view text, OrderType& out) noexcept {
  const std::string value = upper(text);
  if (value == "LIMIT") {
    out = OrderType::kLimit;
    return true;
  }
  if (value == "MARKET") {
    out = OrderType::kMarket;
    return true;
  }
  return false;
}

bool parseTimeInForce(std::string_view text, TimeInForce& out) noexcept {
  const std::string value = upper(text);
  if (value == "GTC") {
    out = TimeInForce::kGoodTilCancel;
    return true;
  }
  if (value == "IOC") {
    out = TimeInForce::kImmediateOrCancel;
    return true;
  }
  if (value == "FOK") {
    out = TimeInForce::kFillOrKill;
    return true;
  }
  return false;
}

std::string formatPrice(Price price) {
  const bool negative = price < 0;
  const std::int64_t magnitude = negative ? -price : price;
  const std::int64_t whole = magnitude / kPriceScale;
  const std::int64_t fraction = magnitude % kPriceScale;

  std::array<char, 48> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%s%lld.%04lld", negative ? "-" : "",
                static_cast<long long>(whole), static_cast<long long>(fraction));
  std::string out(buffer.data());

  // Trim trailing zeros but always keep two decimals, so 12.5000 prints as
  // 12.50 rather than 12.5 -- consistent width matters in a depth ladder.
  while (out.size() > 1 && out.back() == '0' && out[out.size() - 3] != '.') {
    out.pop_back();
  }
  return out;
}

bool parsePrice(std::string_view text, Price& out) {
  if (text.empty()) {
    return false;
  }
  bool negative = false;
  std::size_t cursor = 0;
  if (text[0] == '-' || text[0] == '+') {
    negative = text[0] == '-';
    cursor = 1;
  }

  std::int64_t whole = 0;
  std::size_t digits = 0;
  while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
    whole = whole * 10 + (text[cursor] - '0');
    ++cursor;
    ++digits;
    if (whole > 900000000000LL) {
      return false;  // would overflow once scaled
    }
  }
  if (digits == 0) {
    return false;
  }

  std::int64_t fraction = 0;
  std::int64_t divisor = kPriceScale;
  if (cursor < text.size() && text[cursor] == '.') {
    ++cursor;
    while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
      if (divisor > 1) {
        divisor /= 10;
        fraction += static_cast<std::int64_t>(text[cursor] - '0') * divisor;
      }
      // Extra precision beyond the tick size is truncated, not rounded --
      // rounding a price up could cross a spread the client did not intend.
      ++cursor;
    }
  }
  if (cursor != text.size()) {
    return false;
  }

  const std::int64_t scaled = whole * kPriceScale + fraction;
  out = negative ? -scaled : scaled;
  return true;
}

// ---------------------------------------------------------------------------
// OrderBook
// ---------------------------------------------------------------------------

OrderBook::OrderBook(std::string symbol, std::size_t initial_pool_capacity)
    : symbol_(std::move(symbol)), pool_(initial_pool_capacity) {}

OrderBook::~OrderBook() {
  // Return every resting order to the pool so its destructor runs before the
  // pool's storage goes away.
  for (auto& [id, order] : index_) {
    pool_.release(order);
  }
  index_.clear();
}

bool OrderBook::crosses(Side aggressor, Price price, Price resting, OrderType type) noexcept {
  if (type == OrderType::kMarket) {
    return true;  // a market order accepts whatever the book offers
  }
  return aggressor == Side::kBuy ? price >= resting : price <= resting;
}

void OrderBook::pushBack(PriceLevel& level, Order* order) {
  order->prev = level.tail;
  order->next = nullptr;
  if (level.tail != nullptr) {
    level.tail->next = order;
  } else {
    level.head = order;
  }
  level.tail = order;
  level.total_quantity += order->remaining();
  ++level.order_count;
}

void OrderBook::detach(Order* order) {
  if (order == nullptr) {
    return;
  }

  auto adjust = [&](auto& ladder) {
    auto it = ladder.find(order->price);
    if (it == ladder.end()) {
      return;
    }
    PriceLevel& level = it->second;
    if (order->prev != nullptr) {
      order->prev->next = order->next;
    } else {
      level.head = order->next;
    }
    if (order->next != nullptr) {
      order->next->prev = order->prev;
    } else {
      level.tail = order->prev;
    }
    level.total_quantity -= order->remaining();
    if (level.total_quantity < 0) {
      level.total_quantity = 0;
    }
    if (level.order_count > 0) {
      --level.order_count;
    }
  };

  if (order->side == Side::kBuy) {
    adjust(bids_);
  } else {
    adjust(asks_);
  }

  index_.erase(order->id);
  pool_.release(order);
}

void OrderBook::pruneLevel(Side side, Price price) {
  if (side == Side::kBuy) {
    auto it = bids_.find(price);
    if (it != bids_.end() && it->second.empty()) {
      bids_.erase(it);
    }
  } else {
    auto it = asks_.find(price);
    if (it != asks_.end() && it->second.empty()) {
      asks_.erase(it);
    }
  }
}

Quantity OrderBook::availableLiquidity(const Order& incoming) const {
  Quantity total = 0;
  auto scan = [&](const auto& ladder) {
    for (const auto& [price, level] : ladder) {
      if (!crosses(incoming.side, incoming.price, price, incoming.type)) {
        break;  // ladders are price-ordered, so the first miss ends the walk
      }
      total += level.total_quantity;
      if (total >= incoming.quantity) {
        break;
      }
    }
  };

  if (incoming.side == Side::kBuy) {
    scan(asks_);
  } else {
    scan(bids_);
  }
  return total;
}

Quantity OrderBook::match(Order& incoming, std::vector<Trade>& trades) {
  Quantity total = 0;
  const std::int64_t now = nowNanos();

  auto sweep = [&](auto& ladder) {
    while (incoming.remaining() > 0 && !ladder.empty()) {
      auto level_it = ladder.begin();
      const Price level_price = level_it->first;
      PriceLevel& level = level_it->second;

      if (!crosses(incoming.side, incoming.price, level_price, incoming.type)) {
        return;  // best opposite price no longer crosses; nothing deeper will
      }

      // Time priority within the level: always take from the head.
      while (incoming.remaining() > 0 && level.head != nullptr) {
        Order* resting = level.head;
        const Quantity fill = std::min(incoming.remaining(), resting->remaining());

        incoming.filled += fill;
        resting->filled += fill;
        level.total_quantity -= fill;
        total += fill;

        Trade trade;
        trade.sequence = ++trade_sequence_;
        // The trade prints at the *resting* order's price. Any price
        // improvement therefore accrues to the order that was patient enough
        // to sit on the book, which is the incentive every venue wants.
        trade.price = level_price;
        trade.quantity = fill;
        trade.resting_order_id = resting->id;
        trade.aggressing_order_id = incoming.id;
        trade.aggressor_side = incoming.side;
        trade.timestamp_ns = now;
        trades.push_back(trade);

        total_volume_ += fill;
        last_trade_price_ = level_price;

        if (resting->isFilled()) {
          resting->status = OrderStatus::kFilled;
          detach(resting);
        } else {
          resting->status = OrderStatus::kPartiallyFilled;
        }
      }

      if (level.empty()) {
        ladder.erase(level_it);
      }
    }
  };

  if (incoming.side == Side::kBuy) {
    sweep(asks_);
  } else {
    sweep(bids_);
  }
  return total;
}

void OrderBook::rest(const Order& incoming) {
  Order* stored = pool_.acquire(incoming);
  stored->prev = nullptr;
  stored->next = nullptr;

  if (stored->side == Side::kBuy) {
    PriceLevel& level = bids_[stored->price];
    level.price = stored->price;
    pushBack(level, stored);
  } else {
    PriceLevel& level = asks_[stored->price];
    level.price = stored->price;
    pushBack(level, stored);
  }
  index_.emplace(stored->id, stored);
}

ExecutionReport OrderBook::submit(const Order& request) {
  ExecutionReport report;
  report.order_id = request.id;

  // ---- validation ------------------------------------------------------
  if (request.quantity <= 0) {
    report.status = OrderStatus::kRejected;
    report.reject_reason = "quantity must be positive";
    return report;
  }
  if (request.type == OrderType::kLimit && request.price <= 0) {
    report.status = OrderStatus::kRejected;
    report.reject_reason = "limit price must be positive";
    return report;
  }
  if (index_.find(request.id) != index_.end()) {
    report.status = OrderStatus::kRejected;
    report.reject_reason = "duplicate order id";
    return report;
  }

  Order incoming = request;
  incoming.filled = 0;
  incoming.status = OrderStatus::kNew;
  incoming.prev = nullptr;
  incoming.next = nullptr;
  incoming.sequence = ++arrival_sequence_;
  if (incoming.timestamp_ns == 0) {
    incoming.timestamp_ns = nowNanos();
  }

  // ---- fill-or-kill precheck -------------------------------------------
  if (incoming.time_in_force == TimeInForce::kFillOrKill &&
      availableLiquidity(incoming) < incoming.quantity) {
    report.status = OrderStatus::kCancelled;
    report.remaining = incoming.quantity;
    report.reject_reason = "fill-or-kill could not be filled in full";
    return report;
  }

  // ---- match -----------------------------------------------------------
  match(incoming, report.trades);

  if (!report.trades.empty()) {
    // Volume-weighted average, computed in integer arithmetic so the report
    // never disagrees with the sum of its own trades.
    std::int64_t notional = 0;
    Quantity quantity = 0;
    for (const Trade& trade : report.trades) {
      notional += trade.price * trade.quantity;
      quantity += trade.quantity;
    }
    report.average_price = quantity > 0 ? notional / quantity : 0;
  }

  // ---- rest the remainder ----------------------------------------------
  const Quantity leftover = incoming.remaining();
  if (leftover > 0) {
    const bool may_rest =
        incoming.type == OrderType::kLimit && incoming.time_in_force == TimeInForce::kGoodTilCancel;
    if (may_rest) {
      incoming.status = incoming.filled > 0 ? OrderStatus::kPartiallyFilled : OrderStatus::kNew;
      rest(incoming);
    } else {
      // Market orders never rest; IOC explicitly cancels what it cannot fill
      // immediately.
      incoming.status = OrderStatus::kCancelled;
    }
  } else {
    incoming.status = OrderStatus::kFilled;
  }

  report.status = incoming.status;
  report.filled = incoming.filled;
  report.remaining = incoming.remaining();
  return report;
}

bool OrderBook::cancel(OrderId id, ExecutionReport* report) {
  auto it = index_.find(id);
  if (it == index_.end()) {
    return false;
  }

  Order* order = it->second;
  if (report != nullptr) {
    report->order_id = id;
    report->status = OrderStatus::kCancelled;
    report->filled = order->filled;
    report->remaining = order->remaining();
  }

  const Side side = order->side;
  const Price price = order->price;
  detach(order);
  pruneLevel(side, price);
  return true;
}

ExecutionReport OrderBook::amend(OrderId id, Price new_price, Quantity new_quantity, bool* found) {
  ExecutionReport report;
  report.order_id = id;

  auto it = index_.find(id);
  if (it == index_.end()) {
    if (found != nullptr) {
      *found = false;
    }
    report.status = OrderStatus::kRejected;
    report.reject_reason = "unknown order id";
    return report;
  }
  if (found != nullptr) {
    *found = true;
  }

  Order* resting = it->second;
  const Order original = *resting;

  if (new_quantity <= original.filled) {
    // Amending at or below the filled quantity is a cancel, not an amend.
    cancel(id, &report);
    return report;
  }

  const bool quantity_only_reduction = new_price == original.price && new_quantity < original.quantity;
  if (quantity_only_reduction) {
    // Shrinking in place keeps time priority. This is the one amendment that
    // cannot disadvantage anyone else in the queue, so it is the one case
    // where priority is preserved.
    auto& ladder_level = original.side == Side::kBuy ? bids_[original.price] : asks_[original.price];
    ladder_level.total_quantity -= (original.quantity - new_quantity);
    resting->quantity = new_quantity;
    report.status = resting->isFilled() ? OrderStatus::kFilled : OrderStatus::kPartiallyFilled;
    report.filled = resting->filled;
    report.remaining = resting->remaining();
    return report;
  }

  // Any other change -- a new price, or more size -- loses priority. Otherwise
  // an order could be repriced to the front of a queue it never waited in.
  const Side side = original.side;
  const Price price = original.price;
  detach(resting);
  pruneLevel(side, price);

  Order replacement = original;
  replacement.price = new_price;
  replacement.quantity = new_quantity;
  replacement.prev = nullptr;
  replacement.next = nullptr;
  return submit(replacement);
}

std::optional<Price> OrderBook::bestBid() const noexcept {
  if (bids_.empty()) {
    return std::nullopt;
  }
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const noexcept {
  if (asks_.empty()) {
    return std::nullopt;
  }
  return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const noexcept {
  const std::optional<Price> bid = bestBid();
  const std::optional<Price> ask = bestAsk();
  if (!bid.has_value() || !ask.has_value()) {
    return std::nullopt;
  }
  return *ask - *bid;
}

Quantity OrderBook::quantityAt(Side side, Price price) const {
  if (side == Side::kBuy) {
    auto it = bids_.find(price);
    return it == bids_.end() ? 0 : it->second.total_quantity;
  }
  auto it = asks_.find(price);
  return it == asks_.end() ? 0 : it->second.total_quantity;
}

BookSnapshot OrderBook::snapshot(std::size_t depth) const {
  BookSnapshot out;
  out.symbol = symbol_;
  out.bids.reserve(std::min(depth, bids_.size()));
  out.asks.reserve(std::min(depth, asks_.size()));

  for (const auto& [price, level] : bids_) {
    if (out.bids.size() >= depth) {
      break;
    }
    out.bids.push_back(DepthLevel{price, level.total_quantity, level.order_count});
  }
  for (const auto& [price, level] : asks_) {
    if (out.asks.size() >= depth) {
      break;
    }
    out.asks.push_back(DepthLevel{price, level.total_quantity, level.order_count});
  }
  return out;
}

}  // namespace bourse::match
