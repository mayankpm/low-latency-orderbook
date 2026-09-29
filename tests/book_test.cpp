// Matching rules, one behaviour per test. Every test also checks the book's
// internal invariants afterwards.
#include <string>
#include <vector>

#include "ob/order_book.h"
#include "test.h"

using namespace ob;

namespace {

struct Recorder {
  std::vector<Trade> trades;
  std::vector<std::pair<OrderId, Qty>> cancels;
  std::vector<std::pair<OrderId, Reject>> rejects;
  void OnTrade(const Trade& t) { trades.push_back(t); }
  void OnRest(OrderId, Side, Price, Qty) {}
  void OnCancel(OrderId id, Qty q) { cancels.emplace_back(id, q); }
  void OnReject(OrderId id, Reject r) { rejects.emplace_back(id, r); }
};

struct Fixture {
  Recorder rec;
  OrderBook<Recorder> book{BookConfig{1, 1000, 1000}, rec};
  ~Fixture() noexcept(false) {
    std::string err;
    if (!book.CheckInvariants(&err)) throw obtest::Failure("invariant violated: " + err);
  }
};

void ExpectTrade(const Trade& t, OrderId maker, OrderId taker, Price price, Qty qty) {
  CHECK_EQ(t.maker, maker);
  CHECK_EQ(t.taker, taker);
  CHECK_EQ(t.price, price);
  CHECK_EQ(t.qty, qty);
}

}  // namespace

TEST(book, rests_and_reports_best_prices) {
  Fixture f;
  CHECK(!f.book.BestBid() && !f.book.BestAsk());
  CHECK(f.book.AddLimit(1, Side::kBuy, 100, 10));
  CHECK(f.book.AddLimit(2, Side::kBuy, 101, 5));
  CHECK(f.book.AddLimit(3, Side::kSell, 105, 7));
  CHECK(f.book.AddLimit(4, Side::kSell, 104, 3));
  CHECK_EQ(*f.book.BestBid(), 101);
  CHECK_EQ(*f.book.BestAsk(), 104);
  CHECK_EQ(f.book.VolumeAt(Side::kBuy, 100), 10u);
  CHECK_EQ(f.book.OrderCount(), 4u);
  CHECK(f.rec.trades.empty());
}

TEST(book, price_time_priority) {
  Fixture f;
  f.book.AddLimit(1, Side::kSell, 100, 5);
  f.book.AddLimit(2, Side::kSell, 100, 5);
  f.book.AddLimit(3, Side::kSell, 99, 5);  // Better price arrives later but fills first.
  f.book.AddLimit(10, Side::kBuy, 100, 12);
  CHECK_EQ(f.rec.trades.size(), 3u);
  ExpectTrade(f.rec.trades[0], 3, 10, 99, 5);
  ExpectTrade(f.rec.trades[1], 1, 10, 100, 5);  // Then oldest at 100.
  ExpectTrade(f.rec.trades[2], 2, 10, 100, 2);
  CHECK_EQ(f.book.QueueAt(Side::kSell, 100), (std::vector<std::pair<OrderId, Qty>>{{2, 3}}));
  CHECK(!f.book.BestBid());  // Taker fully filled: nothing rests.
}

TEST(book, sweeps_levels_and_rests_remainder_at_limit) {
  Fixture f;
  f.book.AddLimit(1, Side::kBuy, 100, 5);
  f.book.AddLimit(2, Side::kBuy, 99, 5);
  f.book.AddLimit(3, Side::kBuy, 98, 5);
  f.book.AddLimit(10, Side::kSell, 99, 20);  // Takes 100 and 99, not 98.
  CHECK_EQ(f.rec.trades.size(), 2u);
  CHECK_EQ(*f.book.BestAsk(), 99);
  CHECK_EQ(f.book.VolumeAt(Side::kSell, 99), 10u);
  CHECK_EQ(*f.book.BestBid(), 98);
}

TEST(book, cancel_from_middle_of_queue) {
  Fixture f;
  for (OrderId id = 1; id <= 5; id++) f.book.AddLimit(id, Side::kBuy, 50, id);
  CHECK(f.book.Cancel(3));
  CHECK(f.book.Cancel(1));
  CHECK(f.book.Cancel(5));
  CHECK_EQ(f.book.QueueAt(Side::kBuy, 50), (std::vector<std::pair<OrderId, Qty>>{{2, 2}, {4, 4}}));
  CHECK_EQ(f.book.VolumeAt(Side::kBuy, 50), 6u);
  CHECK(!f.book.Cancel(3));
  CHECK_EQ(f.rec.rejects.back().second, Reject::kUnknownOrder);
  CHECK(f.book.Cancel(2) && f.book.Cancel(4));
  CHECK(!f.book.BestBid());
}

TEST(book, best_price_moves_when_best_level_is_cancelled) {
  Fixture f;
  f.book.AddLimit(1, Side::kSell, 200, 1);
  f.book.AddLimit(2, Side::kSell, 500, 1);
  f.book.AddLimit(3, Side::kBuy, 150, 1);
  f.book.AddLimit(4, Side::kBuy, 10, 1);
  f.book.Cancel(1);
  CHECK_EQ(*f.book.BestAsk(), 500);
  f.book.Cancel(3);
  CHECK_EQ(*f.book.BestBid(), 10);
}

TEST(book, modify_reduce_keeps_priority_other_changes_lose_it) {
  Fixture f;
  f.book.AddLimit(1, Side::kSell, 100, 10);
  f.book.AddLimit(2, Side::kSell, 100, 10);
  CHECK(f.book.Modify(1, 100, 4));  // Reduce: still first.
  CHECK_EQ(f.book.QueueAt(Side::kSell, 100), (std::vector<std::pair<OrderId, Qty>>{{1, 4}, {2, 10}}));
  CHECK(f.book.Modify(1, 100, 8));  // Increase: goes to the back.
  CHECK_EQ(f.book.QueueAt(Side::kSell, 100), (std::vector<std::pair<OrderId, Qty>>{{2, 10}, {1, 8}}));
  f.book.AddLimit(3, Side::kBuy, 95, 5);
  CHECK(f.book.Modify(3, 100, 5));  // Re-price across the spread: trades immediately.
  CHECK_EQ(f.rec.trades.size(), 1u);
  ExpectTrade(f.rec.trades[0], 2, 3, 100, 5);
  CHECK(f.book.Modify(2, 100, 0));  // Quantity 0 cancels.
  CHECK_EQ(f.book.QueueAt(Side::kSell, 100), (std::vector<std::pair<OrderId, Qty>>{{1, 8}}));
  CHECK(!f.book.Modify(1, 5000, 1));  // Out of band: rejected, order unchanged.
  CHECK_EQ(f.book.VolumeAt(Side::kSell, 100), 8u);
}

TEST(book, ioc_and_market_never_rest) {
  Fixture f;
  f.book.AddLimit(1, Side::kSell, 100, 5);
  f.book.AddLimit(2, Side::kSell, 101, 5);
  CHECK(f.book.AddLimit(10, Side::kBuy, 100, 8, TimeInForce::kIoc));
  CHECK_EQ(f.rec.trades.size(), 1u);
  CHECK_EQ(f.rec.cancels.back(), (std::pair<OrderId, Qty>{10, 3}));
  CHECK(!f.book.BestBid());
  CHECK(f.book.AddMarket(11, Side::kBuy, 20));  // Takes everything left, cancels the rest.
  CHECK_EQ(f.rec.trades.size(), 2u);
  ExpectTrade(f.rec.trades[1], 2, 11, 101, 5);
  CHECK_EQ(f.rec.cancels.back(), (std::pair<OrderId, Qty>{11, 15}));
  CHECK(!f.book.BestAsk() && !f.book.BestBid());
}

TEST(book, fill_or_kill_is_all_or_nothing) {
  Fixture f;
  f.book.AddLimit(1, Side::kBuy, 100, 5);
  f.book.AddLimit(2, Side::kBuy, 99, 5);
  CHECK(!f.book.AddLimit(10, Side::kSell, 99, 11, TimeInForce::kFok));  // Only 10 available.
  CHECK_EQ(f.rec.rejects.back().second, Reject::kFokNotFillable);
  CHECK(f.rec.trades.empty());
  CHECK(!f.book.AddLimit(11, Side::kSell, 100, 6, TimeInForce::kFok));  // Only 5 at or above 100.
  CHECK(f.book.AddLimit(12, Side::kSell, 99, 10, TimeInForce::kFok));
  CHECK_EQ(f.rec.trades.size(), 2u);
  CHECK(!f.book.BestBid());
}

TEST(book, rejects_invalid_orders) {
  Fixture f;
  CHECK(!f.book.AddLimit(0, Side::kBuy, 100, 1));
  CHECK(!f.book.AddLimit(1, Side::kBuy, 100, 0));
  CHECK(!f.book.AddLimit(1, Side::kBuy, 0, 1));
  CHECK(!f.book.AddLimit(1, Side::kBuy, 1001, 1));
  CHECK(f.book.AddLimit(1, Side::kBuy, 100, 1));
  CHECK(!f.book.AddLimit(1, Side::kSell, 200, 1));  // Duplicate id.
  CHECK(!f.book.AddMarket(1, Side::kSell, 1));
  const std::vector<Reject> want = {Reject::kInvalidId, Reject::kInvalidQuantity, Reject::kPriceOutOfRange,
                                    Reject::kPriceOutOfRange, Reject::kDuplicateId, Reject::kDuplicateId};
  CHECK_EQ(f.rec.rejects.size(), want.size());
  for (size_t i = 0; i < want.size(); i++) CHECK_EQ(f.rec.rejects[i].second, want[i]);
}

TEST(book, rejects_when_order_pool_is_full) {
  Recorder rec;
  OrderBook<Recorder> book(BookConfig{1, 100, 3}, rec);
  CHECK(book.AddLimit(1, Side::kBuy, 10, 1));
  CHECK(book.AddLimit(2, Side::kBuy, 10, 1));
  CHECK(book.AddLimit(3, Side::kBuy, 10, 1));
  CHECK(!book.AddLimit(4, Side::kBuy, 10, 1));
  CHECK_EQ(rec.rejects.back().second, Reject::kBookFull);
  CHECK(book.AddLimit(5, Side::kSell, 10, 1));  // Trades without resting: allowed.
  CHECK(book.AddLimit(6, Side::kBuy, 10, 1));   // A node was freed.
  std::string err;
  CHECK(book.CheckInvariants(&err));
}

TEST(book, extreme_prices_at_band_edges) {
  Fixture f;
  f.book.AddLimit(1, Side::kBuy, 1, 5);
  f.book.AddLimit(2, Side::kSell, 1000, 5);
  CHECK(f.book.AddMarket(3, Side::kSell, 5));
  CHECK(f.book.AddMarket(4, Side::kBuy, 5));
  CHECK_EQ(f.rec.trades.size(), 2u);
  CHECK(!f.book.BestBid() && !f.book.BestAsk());
}
