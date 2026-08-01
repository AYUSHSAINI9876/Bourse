#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bourse::match {

using OrderId = std::uint64_t;
using SequenceNumber = std::uint64_t;

/// Prices are integer ticks, never floating point.
///
/// A double cannot represent 0.10 exactly, so `0.1 + 0.2 != 0.3` and two
/// orders that should cross at the same price compare unequal. Every exchange
/// that has ever shipped uses scaled integers; `kPriceScale` fixes the
/// granularity at four decimal places.
using Price = std::int64_t;
using Quantity = std::int64_t;

inline constexpr std::int64_t kPriceScale = 10000;

[[nodiscard]] inline constexpr Price priceFromTicks(std::int64_t ticks) noexcept { return ticks; }
[[nodiscard]] std::string formatPrice(Price price);
[[nodiscard]] bool parsePrice(std::string_view text, Price& out);

enum class Side : std::uint8_t { kBuy, kSell };
enum class OrderType : std::uint8_t { kLimit, kMarket };

/// How long an order may rest before it must be resolved.
///
/// Modelled as a value rather than a polymorphic strategy object: the three
/// behaviours differ only in *when* the engine gives up, which is two
/// comparisons in the matching loop. A class hierarchy here would add a
/// virtual call to the hot path to express a two-bit enum.
enum class TimeInForce : std::uint8_t {
  kGoodTilCancel,      ///< rests on the book until filled or cancelled
  kImmediateOrCancel,  ///< fills what it can immediately, cancels the rest
  kFillOrKill,         ///< fills completely and immediately, or not at all
};

enum class OrderStatus : std::uint8_t {
  kNew,
  kPartiallyFilled,
  kFilled,
  kCancelled,
  kRejected,
};

[[nodiscard]] const char* toString(Side side) noexcept;
[[nodiscard]] const char* toString(OrderType type) noexcept;
[[nodiscard]] const char* toString(TimeInForce tif) noexcept;
[[nodiscard]] const char* toString(OrderStatus status) noexcept;
[[nodiscard]] bool parseSide(std::string_view text, Side& out) noexcept;
[[nodiscard]] bool parseOrderType(std::string_view text, OrderType& out) noexcept;
[[nodiscard]] bool parseTimeInForce(std::string_view text, TimeInForce& out) noexcept;

/// A resting or in-flight order.
///
/// Carries its own intrusive list links. The alternative -- a
/// `std::list<Order>` per price level -- allocates a separate node per order
/// and puts a pointer chase between the node and the order data. Here the
/// links live inside the object, the object comes from an ObjectPool, and
/// cancelling is an O(1) unlink with no search.
struct Order {
  OrderId id = 0;
  Side side = Side::kBuy;
  OrderType type = OrderType::kLimit;
  TimeInForce time_in_force = TimeInForce::kGoodTilCancel;
  OrderStatus status = OrderStatus::kNew;

  Price price = 0;
  Quantity quantity = 0;
  Quantity filled = 0;

  std::int64_t timestamp_ns = 0;
  std::uint64_t sequence = 0;  ///< arrival order, breaks price ties

  /// Intrusive FIFO links within one price level. Owned by the OrderBook.
  Order* prev = nullptr;
  Order* next = nullptr;

  [[nodiscard]] Quantity remaining() const noexcept { return quantity - filled; }
  [[nodiscard]] bool isFilled() const noexcept { return filled >= quantity; }
  [[nodiscard]] bool isTerminal() const noexcept {
    return status == OrderStatus::kFilled || status == OrderStatus::kCancelled ||
           status == OrderStatus::kRejected;
  }
};

/// An execution. Immutable once produced.
struct Trade {
  SequenceNumber sequence = 0;
  Price price = 0;
  Quantity quantity = 0;
  OrderId resting_order_id = 0;
  OrderId aggressing_order_id = 0;
  /// Which side removed liquidity. Determines the maker/taker fee split in a
  /// real venue, and tells a market-data consumer whether the print was a buy
  /// or a sell.
  Side aggressor_side = Side::kBuy;
  std::int64_t timestamp_ns = 0;
};

/// The acknowledgement a client receives for every order action.
struct ExecutionReport {
  OrderId order_id = 0;
  OrderStatus status = OrderStatus::kNew;
  Quantity filled = 0;
  Quantity remaining = 0;
  /// Volume-weighted average price across this action's fills. Zero when
  /// nothing traded.
  Price average_price = 0;
  std::string reject_reason;
  std::vector<Trade> trades;
};

}  // namespace bourse::match
