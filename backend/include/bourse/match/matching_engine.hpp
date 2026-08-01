#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bourse/match/order_book.hpp"

namespace bourse::match {

struct EngineStats {
  std::uint64_t orders_accepted = 0;
  std::uint64_t orders_rejected = 0;
  std::uint64_t orders_cancelled = 0;
  std::uint64_t trades_executed = 0;
  std::uint64_t volume_traded = 0;
  std::size_t symbols = 0;
  std::size_t resting_orders = 0;
};

/// Multi-symbol matching engine.
///
/// **Observer, not callback-into-the-network.** Trades and book updates are
/// published to registered observers rather than written to a socket. The
/// engine therefore has no dependency on the network layer at all, which is
/// what lets the whole thing be unit-tested with no sockets and later fanned
/// out over both WebSocket and pub/sub without touching matching logic.
///
/// **Locking.** One mutex guards the symbol table and every book. Each
/// individual OrderBook is deliberately not thread-safe -- concurrency in a
/// real venue comes from sharding symbols across engines pinned to separate
/// cores, not from locking a book. A single mutex is the correct starting
/// point: it is obviously right, and symbol sharding is a mechanical change
/// once there is a measurement showing it matters.
///
/// Observers are always invoked *outside* the lock. Holding it across a
/// callback would serialise every order behind the slowest market-data
/// consumer, and would deadlock outright if an observer called back in.
class MatchingEngine {
 public:
  using TradeObserver = std::function<void(const std::string& symbol, const Trade&)>;
  using BookObserver = std::function<void(const BookSnapshot&)>;

  explicit MatchingEngine(std::size_t trade_history_per_symbol = 512);

  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;

  /// Server-assigned monotonic order id. Clients never choose their own: two
  /// clients picking the same id would silently collide in the book index.
  [[nodiscard]] OrderId nextOrderId() noexcept {
    return next_order_id_.fetch_add(1, std::memory_order_relaxed);
  }

  ExecutionReport submit(const std::string& symbol, const Order& order);
  bool cancel(const std::string& symbol, OrderId id, ExecutionReport* report = nullptr);
  ExecutionReport amend(const std::string& symbol, OrderId id, Price new_price, Quantity new_quantity,
                        bool* found);

  [[nodiscard]] std::optional<BookSnapshot> snapshot(const std::string& symbol,
                                                     std::size_t depth = 10) const;
  [[nodiscard]] std::vector<Trade> recentTrades(const std::string& symbol, std::size_t limit) const;
  [[nodiscard]] std::vector<std::string> symbols() const;
  [[nodiscard]] bool hasSymbol(const std::string& symbol) const;
  [[nodiscard]] std::optional<Price> lastTradePrice(const std::string& symbol) const;

  void addTradeObserver(TradeObserver observer);
  void addBookObserver(BookObserver observer);

  [[nodiscard]] EngineStats stats() const;

  /// Test and admin helper: drops every book and all history.
  void reset();

 private:
  /// Creates the book on first use. Caller must hold `mutex_`.
  OrderBook& bookFor(const std::string& symbol);

  void recordTrades(const std::string& symbol, const std::vector<Trade>& trades);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;
  std::unordered_map<std::string, std::deque<Trade>> trade_history_;
  std::size_t trade_history_limit_;

  std::vector<TradeObserver> trade_observers_;
  std::vector<BookObserver> book_observers_;

  std::atomic<OrderId> next_order_id_{1};
  std::atomic<std::uint64_t> orders_accepted_{0};
  std::atomic<std::uint64_t> orders_rejected_{0};
  std::atomic<std::uint64_t> orders_cancelled_{0};
  std::atomic<std::uint64_t> trades_executed_{0};
  std::atomic<std::uint64_t> volume_traded_{0};
};

}  // namespace bourse::match
