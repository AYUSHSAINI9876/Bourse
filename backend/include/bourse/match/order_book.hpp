#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bourse/core/object_pool.hpp"
#include "bourse/match/order.hpp"

namespace bourse::match {

/// One price level: a FIFO queue of resting orders at a single price.
///
/// The queue is an intrusive doubly-linked list so that cancel is O(1) -- the
/// order object already knows its neighbours, so there is nothing to search
/// for. `total_quantity` is maintained incrementally because market-data
/// depth snapshots ask for it far more often than the level changes.
struct PriceLevel {
  Price price = 0;
  Order* head = nullptr;  ///< oldest, matches first
  Order* tail = nullptr;  ///< newest
  Quantity total_quantity = 0;
  std::size_t order_count = 0;

  [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
};

/// Depth-of-book snapshot entry.
struct DepthLevel {
  Price price = 0;
  Quantity quantity = 0;
  std::size_t orders = 0;
};

struct BookSnapshot {
  std::string symbol;
  std::vector<DepthLevel> bids;  ///< descending price
  std::vector<DepthLevel> asks;  ///< ascending price
};

/// A single-symbol limit order book with price-time priority.
///
/// **Ordering structure.** Bids and asks are `std::map`s keyed by price, giving
/// O(log L) level lookup in L distinct price levels plus the ordered iteration
/// that depth snapshots need. Best bid and best ask are cached so the genuinely
/// hot read -- top of book, consulted on every incoming order -- is O(1) and
/// never walks the tree.
///
/// A production low-latency venue would replace the map with an array-indexed
/// price ladder over a bounded tick range, turning level lookup into a single
/// subtraction and index. That trade is real but not free: it requires knowing
/// the price band up front and wastes memory across sparse books. The map is
/// kept here because it is correct for arbitrary prices and the cached
/// top-of-book removes it from the hot path anyway.
///
/// **Not thread-safe by design.** One book is driven by one thread; concurrency
/// comes from sharding symbols across engines, not from locking a book.
class OrderBook {
 public:
  explicit OrderBook(std::string symbol, std::size_t initial_pool_capacity = 4096);
  ~OrderBook();

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }

  /// Matches `incoming` against the resting book, then rests any remainder if
  /// its time-in-force allows. The Order is copied into pool-owned storage;
  /// the caller keeps ownership of nothing.
  ExecutionReport submit(const Order& incoming);

  /// Removes a resting order. Returns false when the id is unknown, which
  /// covers both "never existed" and "already filled".
  bool cancel(OrderId id, ExecutionReport* report = nullptr);

  /// Cancel-replace. Amending quantity downward keeps time priority; any other
  /// change loses it, because otherwise an order could be repriced to the front
  /// of a queue it never waited in.
  ExecutionReport amend(OrderId id, Price new_price, Quantity new_quantity, bool* found);

  [[nodiscard]] std::optional<Price> bestBid() const noexcept;
  [[nodiscard]] std::optional<Price> bestAsk() const noexcept;
  [[nodiscard]] std::optional<Price> spread() const noexcept;
  [[nodiscard]] Quantity quantityAt(Side side, Price price) const;

  [[nodiscard]] BookSnapshot snapshot(std::size_t depth = 10) const;

  [[nodiscard]] std::size_t restingOrderCount() const noexcept { return index_.size(); }

  [[nodiscard]] std::size_t bidLevelCount() const noexcept { return bids_.size(); }

  [[nodiscard]] std::size_t askLevelCount() const noexcept { return asks_.size(); }

  [[nodiscard]] SequenceNumber lastTradeSequence() const noexcept { return trade_sequence_; }

  [[nodiscard]] Quantity totalVolumeTraded() const noexcept { return total_volume_; }

  [[nodiscard]] std::optional<Price> lastTradePrice() const noexcept { return last_trade_price_; }

  /// Pool statistics, used by the benchmark to assert that steady-state order
  /// entry performs no allocation.
  [[nodiscard]] std::size_t poolChunkCount() const noexcept { return pool_.chunkCount(); }

  [[nodiscard]] std::size_t liveOrderObjects() const noexcept { return pool_.live(); }

 private:
  using BidLadder = std::map<Price, PriceLevel, std::greater<Price>>;
  using AskLadder = std::map<Price, PriceLevel, std::less<Price>>;

  /// Would an aggressive order at `price` cross a resting order at `resting`?
  [[nodiscard]] static bool crosses(Side aggressor, Price price, Price resting, OrderType type) noexcept;

  /// Walks the opposite side filling `incoming`, emitting a Trade per fill.
  Quantity match(Order& incoming, std::vector<Trade>& trades);

  /// Total quantity `incoming` could take right now, without mutating
  /// anything. Fill-or-kill asks this first: it must know whether a complete
  /// fill is possible *before* consuming any liquidity, because a partial fill
  /// it then had to unwind would already have emitted trades.
  [[nodiscard]] Quantity availableLiquidity(const Order& incoming) const;

  void rest(const Order& incoming);
  /// Removes a resting order from its level and the index and returns it to
  /// the pool. Deliberately leaves an emptied level in the ladder so callers
  /// holding an iterator stay valid; `pruneLevel` cleans up afterwards.
  void detach(Order* order);
  void pruneLevel(Side side, Price price);
  static void pushBack(PriceLevel& level, Order* order);

  std::string symbol_;
  BidLadder bids_;
  AskLadder asks_;

  /// id -> resting order. Cancel needs this; without it a cancel would have to
  /// scan every level.
  std::unordered_map<OrderId, Order*> index_;

  ObjectPool<Order> pool_;
  SequenceNumber trade_sequence_ = 0;
  std::uint64_t arrival_sequence_ = 0;
  Quantity total_volume_ = 0;
  std::optional<Price> last_trade_price_;
};

}  // namespace bourse::match
