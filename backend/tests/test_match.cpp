#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "bourse/match/matching_engine.hpp"
#include "bourse/match/order_book.hpp"

using namespace bourse;
using namespace bourse::match;

namespace {

Price px(const char* text) {
  Price value = 0;
  EXPECT_TRUE(parsePrice(text, value)) << "unparsable price literal: " << text;
  return value;
}

Order makeOrder(OrderId id, Side side, Price price, Quantity quantity,
                TimeInForce tif = TimeInForce::kGoodTilCancel, OrderType type = OrderType::kLimit) {
  Order order;
  order.id = id;
  order.side = side;
  order.type = type;
  order.time_in_force = tif;
  order.price = price;
  order.quantity = quantity;
  return order;
}

}  // namespace

// ---------------------------------------------------------------------------
// Price arithmetic
// ---------------------------------------------------------------------------

TEST(PriceTest, ParsesAndFormatsWithoutFloatingPoint) {
  EXPECT_EQ(px("100"), 100 * kPriceScale);
  EXPECT_EQ(px("100.50"), 1005000);
  EXPECT_EQ(px("0.0001"), 1);
  EXPECT_EQ(formatPrice(1005000), "100.50");
  EXPECT_EQ(formatPrice(100 * kPriceScale), "100.00");
  EXPECT_EQ(formatPrice(1), "0.0001");
}

TEST(PriceTest, TheDecimalCaseThatBreaksDoubles) {
  // 0.1 + 0.2 != 0.3 in binary floating point. In integer ticks it is exact,
  // which is why money is never a double here.
  EXPECT_EQ(px("0.1") + px("0.2"), px("0.3"));
}

TEST(PriceTest, RejectsGarbage) {
  Price value = 0;
  EXPECT_FALSE(parsePrice("", value));
  EXPECT_FALSE(parsePrice("abc", value));
  EXPECT_FALSE(parsePrice("12.34.56", value));
  EXPECT_FALSE(parsePrice("12x", value));
}

TEST(EnumTest, ParsesWireSpellings) {
  Side side{};
  EXPECT_TRUE(parseSide("buy", side));
  EXPECT_EQ(side, Side::kBuy);
  EXPECT_TRUE(parseSide("SELL", side));
  EXPECT_EQ(side, Side::kSell);
  EXPECT_FALSE(parseSide("sideways", side));

  TimeInForce tif{};
  EXPECT_TRUE(parseTimeInForce("ioc", tif));
  EXPECT_EQ(tif, TimeInForce::kImmediateOrCancel);
  EXPECT_FALSE(parseTimeInForce("whenever", tif));
}

// ---------------------------------------------------------------------------
// Resting and top of book
// ---------------------------------------------------------------------------

TEST(OrderBookTest, RestingOrdersFormTheBook) {
  OrderBook book("AAPL");
  EXPECT_FALSE(book.bestBid().has_value());
  EXPECT_FALSE(book.bestAsk().has_value());

  book.submit(makeOrder(1, Side::kBuy, px("100.00"), 10));
  book.submit(makeOrder(2, Side::kBuy, px("101.00"), 5));
  book.submit(makeOrder(3, Side::kSell, px("103.00"), 7));
  book.submit(makeOrder(4, Side::kSell, px("102.00"), 3));

  // Best bid is the highest buy, best ask the lowest sell.
  EXPECT_EQ(*book.bestBid(), px("101.00"));
  EXPECT_EQ(*book.bestAsk(), px("102.00"));
  EXPECT_EQ(*book.spread(), px("1.00"));
  EXPECT_EQ(book.restingOrderCount(), 4u);
}

TEST(OrderBookTest, NonCrossingOrdersDoNotTrade) {
  OrderBook book("AAPL");
  ExecutionReport bid = book.submit(makeOrder(1, Side::kBuy, px("100.00"), 10));
  ExecutionReport ask = book.submit(makeOrder(2, Side::kSell, px("101.00"), 10));

  EXPECT_TRUE(bid.trades.empty());
  EXPECT_TRUE(ask.trades.empty());
  EXPECT_EQ(bid.status, OrderStatus::kNew);
  EXPECT_EQ(book.totalVolumeTraded(), 0);
}

// ---------------------------------------------------------------------------
// Matching semantics
// ---------------------------------------------------------------------------

TEST(OrderBookTest, CrossingOrderTradesImmediately) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("100.00"), 10));

  const ExecutionReport report = book.submit(makeOrder(2, Side::kBuy, px("100.00"), 4));
  ASSERT_EQ(report.trades.size(), 1u);
  EXPECT_EQ(report.status, OrderStatus::kFilled);
  EXPECT_EQ(report.filled, 4);
  EXPECT_EQ(report.remaining, 0);
  EXPECT_EQ(report.trades[0].price, px("100.00"));
  EXPECT_EQ(report.trades[0].quantity, 4);
  EXPECT_EQ(report.trades[0].aggressor_side, Side::kBuy);

  // The resting order is partially consumed, not removed.
  EXPECT_EQ(book.quantityAt(Side::kSell, px("100.00")), 6);
}

TEST(OrderBookTest, PriceImprovementGoesToTheRestingOrder) {
  // A buyer willing to pay 101 that meets a seller resting at 100 trades at
  // 100 -- the patient side keeps the improvement. Getting this backwards is
  // a classic exchange bug.
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("100.00"), 10));

  const ExecutionReport report = book.submit(makeOrder(2, Side::kBuy, px("101.00"), 10));
  ASSERT_EQ(report.trades.size(), 1u);
  EXPECT_EQ(report.trades[0].price, px("100.00"));
  EXPECT_EQ(report.average_price, px("100.00"));
}

TEST(OrderBookTest, TimePriorityWithinAPriceLevel) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("100.00"), 5));  // first in
  book.submit(makeOrder(2, Side::kSell, px("100.00"), 5));  // second in

  const ExecutionReport report = book.submit(makeOrder(3, Side::kBuy, px("100.00"), 5));
  ASSERT_EQ(report.trades.size(), 1u);
  EXPECT_EQ(report.trades[0].resting_order_id, 1u) << "FIFO violated: the newer order matched first";
}

TEST(OrderBookTest, PricePriorityBeatsTimePriority) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("101.00"), 5));  // older but worse
  book.submit(makeOrder(2, Side::kSell, px("100.00"), 5));  // newer but better

  const ExecutionReport report = book.submit(makeOrder(3, Side::kBuy, px("101.00"), 5));
  ASSERT_EQ(report.trades.size(), 1u);
  EXPECT_EQ(report.trades[0].resting_order_id, 2u) << "price priority must outrank time priority";
  EXPECT_EQ(report.trades[0].price, px("100.00"));
}

TEST(OrderBookTest, SweepsMultipleLevels) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("100.00"), 3));
  book.submit(makeOrder(2, Side::kSell, px("101.00"), 3));
  book.submit(makeOrder(3, Side::kSell, px("102.00"), 3));

  const ExecutionReport report = book.submit(makeOrder(4, Side::kBuy, px("102.00"), 8));
  ASSERT_EQ(report.trades.size(), 3u);
  EXPECT_EQ(report.trades[0].price, px("100.00"));
  EXPECT_EQ(report.trades[1].price, px("101.00"));
  EXPECT_EQ(report.trades[2].price, px("102.00"));
  EXPECT_EQ(report.filled, 8);
  EXPECT_EQ(report.trades[2].quantity, 2) << "last level should be partially consumed";

  // VWAP over 3@100, 3@101, 2@102.
  const std::int64_t expected = (3 * px("100.00") + 3 * px("101.00") + 2 * px("102.00")) / 8;
  EXPECT_EQ(report.average_price, expected);
}

TEST(OrderBookTest, PartialFillRestsTheRemainder) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("100.00"), 3));

  const ExecutionReport report = book.submit(makeOrder(2, Side::kBuy, px("100.00"), 10));
  EXPECT_EQ(report.status, OrderStatus::kPartiallyFilled);
  EXPECT_EQ(report.filled, 3);
  EXPECT_EQ(report.remaining, 7);
  EXPECT_EQ(*book.bestBid(), px("100.00"));
  EXPECT_EQ(book.quantityAt(Side::kBuy, px("100.00")), 7);
}

// ---------------------------------------------------------------------------
// Order types and time in force
// ---------------------------------------------------------------------------

TEST(OrderBookTest, MarketOrderTakesAnyPriceAndNeverRests) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("500.00"), 2));

  Order market = makeOrder(2, Side::kBuy, 0, 5, TimeInForce::kImmediateOrCancel, OrderType::kMarket);
  const ExecutionReport report = book.submit(market);

  EXPECT_EQ(report.filled, 2);
  EXPECT_EQ(report.status, OrderStatus::kCancelled) << "unfilled market remainder must not rest";
  EXPECT_FALSE(book.bestBid().has_value());
}

TEST(OrderBookTest, ImmediateOrCancelDropsTheRemainder) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("100.00"), 3));

  const ExecutionReport report =
      book.submit(makeOrder(2, Side::kBuy, px("100.00"), 10, TimeInForce::kImmediateOrCancel));
  EXPECT_EQ(report.filled, 3);
  EXPECT_EQ(report.status, OrderStatus::kCancelled);
  EXPECT_FALSE(book.bestBid().has_value()) << "IOC remainder must not rest";
}

TEST(OrderBookTest, FillOrKillIsAllOrNothing) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("100.00"), 3));

  // Not enough liquidity: nothing at all may trade.
  const ExecutionReport rejected =
      book.submit(makeOrder(2, Side::kBuy, px("100.00"), 10, TimeInForce::kFillOrKill));
  EXPECT_TRUE(rejected.trades.empty()) << "FOK must not partially fill before giving up";
  EXPECT_EQ(rejected.status, OrderStatus::kCancelled);
  EXPECT_EQ(book.quantityAt(Side::kSell, px("100.00")), 3) << "resting liquidity must be untouched";

  // Enough liquidity: fills completely.
  const ExecutionReport filled =
      book.submit(makeOrder(3, Side::kBuy, px("100.00"), 3, TimeInForce::kFillOrKill));
  EXPECT_EQ(filled.status, OrderStatus::kFilled);
  EXPECT_EQ(filled.filled, 3);
}

TEST(OrderBookTest, FillOrKillAcrossMultipleLevels) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("100.00"), 2));
  book.submit(makeOrder(2, Side::kSell, px("101.00"), 2));

  const ExecutionReport report =
      book.submit(makeOrder(3, Side::kBuy, px("101.00"), 4, TimeInForce::kFillOrKill));
  EXPECT_EQ(report.status, OrderStatus::kFilled);
  EXPECT_EQ(report.trades.size(), 2u);
}

// ---------------------------------------------------------------------------
// Cancel and amend
// ---------------------------------------------------------------------------

TEST(OrderBookTest, CancelRemovesRestingLiquidity) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kBuy, px("100.00"), 10));
  EXPECT_EQ(book.restingOrderCount(), 1u);

  ExecutionReport report;
  EXPECT_TRUE(book.cancel(1, &report));
  EXPECT_EQ(report.status, OrderStatus::kCancelled);
  EXPECT_EQ(book.restingOrderCount(), 0u);
  EXPECT_FALSE(book.bestBid().has_value());
  EXPECT_EQ(book.bidLevelCount(), 0u) << "an emptied price level must be pruned";
}

TEST(OrderBookTest, CancelOfUnknownOrderIsARefusalNotACrash) {
  OrderBook book("AAPL");
  EXPECT_FALSE(book.cancel(999));
  book.submit(makeOrder(1, Side::kBuy, px("100.00"), 1));
  EXPECT_TRUE(book.cancel(1));
  EXPECT_FALSE(book.cancel(1)) << "double cancel must be refused, not double-freed";
}

TEST(OrderBookTest, ReducingQuantityKeepsTimePriority) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("100.00"), 10));  // first in queue
  book.submit(makeOrder(2, Side::kSell, px("100.00"), 10));

  bool found = false;
  book.amend(1, px("100.00"), 5, &found);
  ASSERT_TRUE(found);
  EXPECT_EQ(book.quantityAt(Side::kSell, px("100.00")), 15);

  // Order 1 shrank but must still be at the front.
  const ExecutionReport report = book.submit(makeOrder(3, Side::kBuy, px("100.00"), 5));
  ASSERT_EQ(report.trades.size(), 1u);
  EXPECT_EQ(report.trades[0].resting_order_id, 1u);
}

TEST(OrderBookTest, RepricingLosesTimePriority) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kSell, px("100.00"), 5));
  book.submit(makeOrder(2, Side::kSell, px("100.00"), 5));

  // Reprice order 1 away and back: it must go to the back of the queue,
  // otherwise an order could jump a queue it never waited in.
  bool found = false;
  book.amend(1, px("101.00"), 5, &found);
  ASSERT_TRUE(found);
  book.amend(1, px("100.00"), 5, &found);

  const ExecutionReport report = book.submit(makeOrder(9, Side::kBuy, px("100.00"), 5));
  ASSERT_EQ(report.trades.size(), 1u);
  EXPECT_EQ(report.trades[0].resting_order_id, 2u) << "repriced order kept priority it should have lost";
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

TEST(OrderBookTest, RejectsMalformedOrders) {
  OrderBook book("AAPL");
  EXPECT_EQ(book.submit(makeOrder(1, Side::kBuy, px("100.00"), 0)).status, OrderStatus::kRejected);
  EXPECT_EQ(book.submit(makeOrder(2, Side::kBuy, px("100.00"), -5)).status, OrderStatus::kRejected);
  EXPECT_EQ(book.submit(makeOrder(3, Side::kBuy, 0, 5)).status, OrderStatus::kRejected);

  book.submit(makeOrder(4, Side::kBuy, px("100.00"), 5));
  EXPECT_EQ(book.submit(makeOrder(4, Side::kBuy, px("100.00"), 5)).status, OrderStatus::kRejected)
      << "duplicate order ids must be refused";
}

// ---------------------------------------------------------------------------
// Depth and pooling
// ---------------------------------------------------------------------------

TEST(OrderBookTest, SnapshotIsOrderedAndAggregated) {
  OrderBook book("AAPL");
  book.submit(makeOrder(1, Side::kBuy, px("100.00"), 5));
  book.submit(makeOrder(2, Side::kBuy, px("100.00"), 7));  // same level
  book.submit(makeOrder(3, Side::kBuy, px("99.00"), 2));
  book.submit(makeOrder(4, Side::kSell, px("101.00"), 4));
  book.submit(makeOrder(5, Side::kSell, px("102.00"), 6));

  const BookSnapshot snapshot = book.snapshot(10);
  ASSERT_EQ(snapshot.bids.size(), 2u);
  EXPECT_EQ(snapshot.bids[0].price, px("100.00"));
  EXPECT_EQ(snapshot.bids[0].quantity, 12) << "orders at one price must aggregate";
  EXPECT_EQ(snapshot.bids[0].orders, 2u);
  EXPECT_GT(snapshot.bids[0].price, snapshot.bids[1].price) << "bids must descend";

  ASSERT_EQ(snapshot.asks.size(), 2u);
  EXPECT_LT(snapshot.asks[0].price, snapshot.asks[1].price) << "asks must ascend";
}

TEST(OrderBookTest, SnapshotRespectsDepthLimit) {
  OrderBook book("AAPL");
  for (int i = 1; i <= 50; ++i) {
    book.submit(makeOrder(static_cast<OrderId>(i), Side::kBuy,
                          px("100.00") - static_cast<Price>(i) * kPriceScale, 1));
  }
  EXPECT_EQ(book.snapshot(5).bids.size(), 5u);
}

TEST(OrderBookTest, SteadyStateOrderEntryStopsAllocating) {
  // This is the property the ObjectPool exists for: after warm-up, submitting
  // and cancelling orders forever must not ask the OS for more memory.
  OrderBook book("AAPL", /*initial_pool_capacity=*/512);
  for (OrderId id = 1; id <= 400; ++id) {
    book.submit(makeOrder(id, Side::kBuy, px("100.00"), 1));
  }
  for (OrderId id = 1; id <= 400; ++id) {
    book.cancel(id);
  }
  const std::size_t chunks_after_warmup = book.poolChunkCount();

  for (OrderId id = 1000; id < 20000; ++id) {
    book.submit(makeOrder(id, Side::kBuy, px("100.00"), 1));
    book.cancel(id);
  }
  EXPECT_EQ(book.poolChunkCount(), chunks_after_warmup)
      << "order entry allocated after warm-up; the pool is not recycling";
  EXPECT_EQ(book.liveOrderObjects(), 0u);
}

// ---------------------------------------------------------------------------
// MatchingEngine
// ---------------------------------------------------------------------------

TEST(MatchingEngineTest, KeepsSymbolsIndependent) {
  MatchingEngine engine;
  Order aapl = makeOrder(engine.nextOrderId(), Side::kBuy, px("100.00"), 5);
  Order msft = makeOrder(engine.nextOrderId(), Side::kSell, px("100.00"), 5);

  engine.submit("AAPL", aapl);
  const ExecutionReport report = engine.submit("MSFT", msft);
  EXPECT_TRUE(report.trades.empty()) << "orders crossed across different symbols";

  EXPECT_EQ(engine.symbols(), (std::vector<std::string>{"AAPL", "MSFT"}));
  EXPECT_TRUE(engine.hasSymbol("AAPL"));
  EXPECT_FALSE(engine.hasSymbol("TSLA"));
}

TEST(MatchingEngineTest, AssignsUniqueOrderIds) {
  MatchingEngine engine;
  std::vector<OrderId> ids;
  for (int i = 0; i < 1000; ++i) {
    ids.push_back(engine.nextOrderId());
  }
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(std::unique(ids.begin(), ids.end()), ids.end()) << "duplicate order id issued";
}

TEST(MatchingEngineTest, NotifiesObserversOfTrades) {
  MatchingEngine engine;
  std::vector<Trade> seen;
  std::vector<std::string> symbols_seen;
  int book_updates = 0;

  engine.addTradeObserver([&](const std::string& symbol, const Trade& trade) {
    symbols_seen.push_back(symbol);
    seen.push_back(trade);
  });
  engine.addBookObserver([&](const BookSnapshot&) { ++book_updates; });

  engine.submit("AAPL", makeOrder(engine.nextOrderId(), Side::kSell, px("100.00"), 5));
  engine.submit("AAPL", makeOrder(engine.nextOrderId(), Side::kBuy, px("100.00"), 5));

  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(symbols_seen[0], "AAPL");
  EXPECT_EQ(seen[0].quantity, 5);
  EXPECT_GE(book_updates, 2);
}

TEST(MatchingEngineTest, KeepsABoundedTradeTape) {
  MatchingEngine engine(/*trade_history_per_symbol=*/8);
  for (int i = 0; i < 30; ++i) {
    engine.submit("AAPL", makeOrder(engine.nextOrderId(), Side::kSell, px("100.00"), 1));
    engine.submit("AAPL", makeOrder(engine.nextOrderId(), Side::kBuy, px("100.00"), 1));
  }
  const std::vector<Trade> tape = engine.recentTrades("AAPL", 100);
  EXPECT_EQ(tape.size(), 8u) << "the tape must be bounded, not an unbounded leak";
  // Most recent first.
  EXPECT_GT(tape.front().sequence, tape.back().sequence);
}

TEST(MatchingEngineTest, TracksStatistics) {
  MatchingEngine engine;
  engine.submit("AAPL", makeOrder(engine.nextOrderId(), Side::kSell, px("100.00"), 10));
  engine.submit("AAPL", makeOrder(engine.nextOrderId(), Side::kBuy, px("100.00"), 4));
  engine.submit("AAPL", makeOrder(engine.nextOrderId(), Side::kBuy, px("100.00"), 0));  // rejected

  const EngineStats stats = engine.stats();
  EXPECT_EQ(stats.orders_accepted, 2u);
  EXPECT_EQ(stats.orders_rejected, 1u);
  EXPECT_EQ(stats.trades_executed, 1u);
  EXPECT_EQ(stats.volume_traded, 4u);
  EXPECT_EQ(stats.symbols, 1u);
  EXPECT_EQ(stats.resting_orders, 1u);
}

TEST(MatchingEngineTest, ConservesQuantityUnderConcurrentSubmission) {
  // Whatever the interleaving, total traded volume can never exceed what was
  // offered, and the book plus the fills must account for every unit.
  MatchingEngine engine;
  constexpr int kThreads = 4;
  constexpr int kOrdersPerThread = 500;

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&engine, t] {
      for (int i = 0; i < kOrdersPerThread; ++i) {
        const Side side = ((t + i) % 2 == 0) ? Side::kBuy : Side::kSell;
        Order order = makeOrder(engine.nextOrderId(), side, px("100.00"), 1);
        engine.submit("AAPL", order);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  const EngineStats stats = engine.stats();
  const std::uint64_t total_units = kThreads * kOrdersPerThread;
  // Each trade consumes one unit from each side.
  EXPECT_EQ(stats.volume_traded * 2 + stats.resting_orders, total_units)
      << "units were lost or created under concurrency";
}
