#include "bourse/match/matching_engine.hpp"

#include <algorithm>
#include <utility>

#include "bourse/core/clock.hpp"
#include "bourse/core/metrics.hpp"

namespace bourse::match {

MatchingEngine::MatchingEngine(std::size_t trade_history_per_symbol)
    : trade_history_limit_(trade_history_per_symbol == 0 ? 1 : trade_history_per_symbol) {}

OrderBook& MatchingEngine::bookFor(const std::string& symbol) {
  auto it = books_.find(symbol);
  if (it == books_.end()) {
    it = books_.emplace(symbol, std::make_unique<OrderBook>(symbol)).first;
  }
  return *it->second;
}

void MatchingEngine::recordTrades(const std::string& symbol, const std::vector<Trade>& trades) {
  std::deque<Trade>& history = trade_history_[symbol];
  for (const Trade& trade : trades) {
    history.push_back(trade);
  }
  // Bounded ring: an unbounded tape is a memory leak with a business
  // justification attached to it.
  while (history.size() > trade_history_limit_) {
    history.pop_front();
  }
}

ExecutionReport MatchingEngine::submit(const std::string& symbol, const Order& order) {
  static Histogram& latency = MetricsRegistry::instance().histogram("bourse_match_latency_nanos");
  const std::int64_t started = nowNanos();

  ExecutionReport report;
  BookSnapshot snapshot_after;
  std::vector<TradeObserver> trade_observers;
  std::vector<BookObserver> book_observers;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    OrderBook& book = bookFor(symbol);
    report = book.submit(order);

    if (report.status == OrderStatus::kRejected) {
      orders_rejected_.fetch_add(1, std::memory_order_relaxed);
    } else {
      orders_accepted_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!report.trades.empty()) {
      trades_executed_.fetch_add(report.trades.size(), std::memory_order_relaxed);
      Quantity volume = 0;
      for (const Trade& trade : report.trades) {
        volume += trade.quantity;
      }
      volume_traded_.fetch_add(static_cast<std::uint64_t>(volume), std::memory_order_relaxed);
      recordTrades(symbol, report.trades);
    }

    snapshot_after = book.snapshot();
    trade_observers = trade_observers_;
    book_observers = book_observers_;
  }

  // Latency is measured over the matching work only, before any observer runs:
  // a slow market-data consumer must not show up as engine latency.
  latency.record(static_cast<std::uint64_t>(nowNanos() - started));

  for (const TradeObserver& observer : trade_observers) {
    for (const Trade& trade : report.trades) {
      observer(symbol, trade);
    }
  }
  if (!report.trades.empty() || report.status != OrderStatus::kRejected) {
    for (const BookObserver& observer : book_observers) {
      observer(snapshot_after);
    }
  }
  return report;
}

bool MatchingEngine::cancel(const std::string& symbol, OrderId id, ExecutionReport* report) {
  BookSnapshot snapshot_after;
  std::vector<BookObserver> book_observers;
  bool cancelled = false;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = books_.find(symbol);
    if (it == books_.end()) {
      return false;
    }
    cancelled = it->second->cancel(id, report);
    if (!cancelled) {
      return false;
    }
    orders_cancelled_.fetch_add(1, std::memory_order_relaxed);
    snapshot_after = it->second->snapshot();
    book_observers = book_observers_;
  }

  for (const BookObserver& observer : book_observers) {
    observer(snapshot_after);
  }
  return true;
}

ExecutionReport MatchingEngine::amend(const std::string& symbol, OrderId id, Price new_price,
                                      Quantity new_quantity, bool* found) {
  ExecutionReport report;
  BookSnapshot snapshot_after;
  std::vector<TradeObserver> trade_observers;
  std::vector<BookObserver> book_observers;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = books_.find(symbol);
    if (it == books_.end()) {
      if (found != nullptr) {
        *found = false;
      }
      report.order_id = id;
      report.status = OrderStatus::kRejected;
      report.reject_reason = "unknown symbol";
      return report;
    }
    report = it->second->amend(id, new_price, new_quantity, found);
    if (!report.trades.empty()) {
      trades_executed_.fetch_add(report.trades.size(), std::memory_order_relaxed);
      recordTrades(symbol, report.trades);
    }
    snapshot_after = it->second->snapshot();
    trade_observers = trade_observers_;
    book_observers = book_observers_;
  }

  for (const TradeObserver& observer : trade_observers) {
    for (const Trade& trade : report.trades) {
      observer(symbol, trade);
    }
  }
  for (const BookObserver& observer : book_observers) {
    observer(snapshot_after);
  }
  return report;
}

std::optional<BookSnapshot> MatchingEngine::snapshot(const std::string& symbol, std::size_t depth) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = books_.find(symbol);
  if (it == books_.end()) {
    return std::nullopt;
  }
  return it->second->snapshot(depth);
}

std::vector<Trade> MatchingEngine::recentTrades(const std::string& symbol, std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = trade_history_.find(symbol);
  if (it == trade_history_.end()) {
    return {};
  }
  const std::deque<Trade>& history = it->second;
  const std::size_t count = std::min(limit, history.size());
  // Most recent first -- that is the order a tape is read in.
  std::vector<Trade> out;
  out.reserve(count);
  for (auto cursor = history.rbegin(); cursor != history.rend() && out.size() < count; ++cursor) {
    out.push_back(*cursor);
  }
  return out;
}

std::vector<std::string> MatchingEngine::symbols() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> out;
  out.reserve(books_.size());
  for (const auto& [symbol, book] : books_) {
    out.push_back(symbol);
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool MatchingEngine::hasSymbol(const std::string& symbol) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return books_.find(symbol) != books_.end();
}

std::optional<Price> MatchingEngine::lastTradePrice(const std::string& symbol) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = books_.find(symbol);
  if (it == books_.end()) {
    return std::nullopt;
  }
  return it->second->lastTradePrice();
}

void MatchingEngine::addTradeObserver(TradeObserver observer) {
  std::lock_guard<std::mutex> lock(mutex_);
  trade_observers_.push_back(std::move(observer));
}

void MatchingEngine::addBookObserver(BookObserver observer) {
  std::lock_guard<std::mutex> lock(mutex_);
  book_observers_.push_back(std::move(observer));
}

EngineStats MatchingEngine::stats() const {
  EngineStats out;
  out.orders_accepted = orders_accepted_.load(std::memory_order_relaxed);
  out.orders_rejected = orders_rejected_.load(std::memory_order_relaxed);
  out.orders_cancelled = orders_cancelled_.load(std::memory_order_relaxed);
  out.trades_executed = trades_executed_.load(std::memory_order_relaxed);
  out.volume_traded = volume_traded_.load(std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(mutex_);
  out.symbols = books_.size();
  for (const auto& [symbol, book] : books_) {
    out.resting_orders += book->restingOrderCount();
  }
  return out;
}

void MatchingEngine::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  books_.clear();
  trade_history_.clear();
}

}  // namespace bourse::match
