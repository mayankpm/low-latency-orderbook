// Differential testing: the optimized OrderBook and the simple ReferenceBook
// process the same message streams and must emit identical events (every
// trade, rest, cancel and reject, in order) and end with identical books.
// Streams cover tight and wide books, heavy cancelling and heavy trading.
#include <cstdio>
#include <string>
#include <vector>

#include "ob/order_book.h"
#include "ob/reference_book.h"
#include "ob/workload.h"
#include "test.h"

using namespace ob;

namespace {

struct Event {
  char kind;
  OrderId a;
  OrderId b;
  Price price;
  uint64_t qty;
  bool operator==(const Event&) const = default;
};

struct EventLog {
  std::vector<Event> events;
  void OnTrade(const Trade& t) { events.push_back({'T', t.maker, t.taker, t.price, t.qty}); }
  void OnRest(OrderId id, Side s, Price p, Qty q) { events.push_back({'R', id, static_cast<OrderId>(s), p, q}); }
  void OnCancel(OrderId id, Qty q) { events.push_back({'C', id, 0, 0, q}); }
  void OnReject(OrderId id, Reject r) { events.push_back({'X', id, static_cast<OrderId>(r), 0, 0}); }
};

std::string Describe(const Event& e) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%c a=%llu b=%llu price=%lld qty=%llu", e.kind,
                static_cast<unsigned long long>(e.a), static_cast<unsigned long long>(e.b),
                static_cast<long long>(e.price), static_cast<unsigned long long>(e.qty));
  return buf;
}

template <typename IdPolicy = SequentialIds>
void RunDifferential(const WorkloadConfig& wc, const BookConfig& bc) {
  const auto messages = GenerateWorkload(wc, bc);
  EventLog fast_log, ref_log;
  OrderBook<EventLog, IdPolicy> fast(bc, fast_log);
  ReferenceBook<EventLog> ref(bc, ref_log);
  size_t trades = 0;
  for (size_t i = 0; i < messages.size(); i++) {
    const size_t before = fast_log.events.size();
    const bool a = Apply(fast, messages[i]);
    const bool b = Apply(ref, messages[i]);
    if (a != b || fast_log.events.size() != ref_log.events.size()) {
      throw obtest::Failure("message " + std::to_string(i) + ": results diverge");
    }
    for (size_t e = before; e < fast_log.events.size(); e++) {
      if (!(fast_log.events[e] == ref_log.events[e])) {
        throw obtest::Failure("message " + std::to_string(i) + ": " + Describe(fast_log.events[e]) + " vs " +
                              Describe(ref_log.events[e]));
      }
      trades += fast_log.events[e].kind == 'T';
    }
    // Keep memory flat: only the per-message comparison needs the events.
    fast_log.events.clear();
    ref_log.events.clear();
    if (i % 200000 == 0) {
      std::string err;
      if (!fast.CheckInvariants(&err)) throw obtest::Failure("invariant: " + err);
    }
  }
  std::string err;
  if (!fast.CheckInvariants(&err)) throw obtest::Failure("invariant: " + err);
  for (Side side : {Side::kBuy, Side::kSell}) {
    const auto a = fast.Depth(side, 1u << 30);
    const auto b = ref.Depth(side, 1u << 30);
    CHECK_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); i++) {
      CHECK(a[i].price == b[i].price && a[i].qty == b[i].qty && a[i].orders == b[i].orders);
    }
  }
  CHECK_EQ(fast.OrderCount(), ref.OrderCount());
  CHECK(trades > messages.size() / 50);  // The stream really exercised matching.
  std::printf("         %zu messages, %zu trades, %zu resting at end: identical\n", messages.size(), trades,
              fast.OrderCount());
}

}  // namespace

TEST(differential, realistic_flow) {
  WorkloadConfig wc;
  wc.seed = 101;
  wc.messages = 2'000'000;
  RunDifferential(wc, BookConfig{1, 100'000, 1 << 20});
}

TEST(differential, tight_book_heavy_trading) {
  WorkloadConfig wc;
  wc.seed = 202;
  wc.messages = 1'000'000;
  wc.depth_decay = 0.8;         // Orders cluster at the touch.
  wc.aggressive_share = 0.25;
  wc.market_share = 0.05;
  wc.fok_share = 0.05;
  wc.target_resting = 500;
  RunDifferential(wc, BookConfig{1, 100'000, 1 << 16});
}

TEST(differential, wide_book_heavy_cancels_and_modifies) {
  WorkloadConfig wc;
  wc.seed = 303;
  wc.messages = 1'000'000;
  wc.depth_decay = 0.02;        // Deep, sparse book.
  wc.cancel_share = 0.5;
  wc.modify_share = 0.3;
  RunDifferential(wc, BookConfig{1, 100'000, 1 << 20});
}

TEST(differential, arbitrary_id_policy) {
  WorkloadConfig wc;
  wc.seed = 505;
  wc.messages = 500'000;
  RunDifferential<ArbitraryIds>(wc, BookConfig{1, 100'000, 1 << 18});
}

TEST(differential, narrow_price_band_hits_edges) {
  WorkloadConfig wc;
  wc.seed = 404;
  wc.messages = 500'000;
  wc.mid = 30;
  wc.depth_decay = 0.1;         // Prices clamp at both band edges.
  wc.target_resting = 2000;
  RunDifferential(wc, BookConfig{1, 64, 1 << 16});
}
