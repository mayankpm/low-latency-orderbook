// Synthetic order flow for tests and benchmarks.
//
// The generator models a single instrument whose mid price drifts in a random
// walk. Most messages are passive limit orders placed a few ticks from the
// mid (geometric distance), with a realistic share of cancels and modifies,
// and a smaller share of marketable limit, market, IOC and FOK orders that
// trade. It runs a real book while generating, so cancels and modifies target
// orders that are actually resting at that point in the stream, the way they
// would in a live market.
#pragma once

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "ob/order_book.h"
#include "ob/types.h"

namespace ob {

enum class MsgType : uint8_t { kLimit, kMarket, kCancel, kModify };

struct Message {
  OrderId id;
  Price price;
  Qty qty;
  MsgType type;
  Side side;
  TimeInForce tif;
};

// Applies one message to any book with the OrderBook interface.
template <typename Book>
OB_ALWAYS_INLINE bool Apply(Book& book, const Message& m) {
  switch (m.type) {
    case MsgType::kLimit: return book.AddLimit(m.id, m.side, m.price, m.qty, m.tif);
    case MsgType::kMarket: return book.AddMarket(m.id, m.side, m.qty);
    case MsgType::kCancel: return book.Cancel(m.id);
    case MsgType::kModify: return book.Modify(m.id, m.price, m.qty);
  }
  return false;
}

struct WorkloadConfig {
  uint64_t seed = 1;
  size_t messages = 1'000'000;
  Price mid = 50'000;              // Starting mid price in ticks.
  double cancel_share = 0.35;
  double modify_share = 0.10;
  double aggressive_share = 0.06;  // Marketable limit orders.
  double market_share = 0.02;
  double ioc_share = 0.02;
  double fok_share = 0.01;
  double depth_decay = 0.25;       // Geometric parameter for distance from mid (smaller = deeper book).
  uint32_t max_lot = 100;
  size_t target_resting = 20'000;  // Below this many resting orders, only add.
};

// Tracks resting orders during generation, driven by the book's events.
class LiveOrders {
 public:
  struct Info {
    Qty qty;
    Price price;
    Side side;
  };

  void OnTrade(const Trade& t) noexcept {
    auto it = info_.find(t.maker);
    if (it == info_.end()) return;
    it->second.qty -= t.qty;
    if (it->second.qty == 0) Remove(t.maker);
  }
  void OnRest(OrderId id, Side side, Price price, Qty qty) noexcept {
    if (!info_.count(id)) {
      pos_[id] = ids_.size();
      ids_.push_back(id);
    }
    info_[id] = Info{qty, price, side};
  }
  void OnCancel(OrderId id, Qty) noexcept {
    if (info_.count(id)) Remove(id);
  }
  void OnReject(OrderId, Reject) noexcept {}

  size_t size() const { return ids_.size(); }
  OrderId Random(std::mt19937_64& rng) const { return ids_[rng() % ids_.size()]; }
  const Info& Get(OrderId id) const { return info_.at(id); }
  void SetQty(OrderId id, Qty q) { info_[id].qty = q; }

  void Remove(OrderId id) {
    info_.erase(id);
    const size_t p = pos_[id];
    pos_[ids_.back()] = p;
    ids_[p] = ids_.back();
    ids_.pop_back();
    pos_.erase(id);
  }

 private:
  std::unordered_map<OrderId, Info> info_;
  std::unordered_map<OrderId, size_t> pos_;
  std::vector<OrderId> ids_;
};

inline std::vector<Message> GenerateWorkload(const WorkloadConfig& cfg, const BookConfig& book_cfg) {
  std::mt19937_64 rng(cfg.seed);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::geometric_distribution<int> distance(cfg.depth_decay);
  LiveOrders live;
  OrderBook<LiveOrders> book(book_cfg, live);

  std::vector<Message> out;
  out.reserve(cfg.messages);
  Price mid = cfg.mid;
  OrderId next_id = 1;
  const Price lo = book_cfg.min_price + 1, hi = book_cfg.max_price - 1;
  auto clamp = [&](Price p) { return p < lo ? lo : (p > hi ? hi : p); };
  auto lot = [&] { return static_cast<Qty>(1 + rng() % cfg.max_lot); };

  while (out.size() < cfg.messages) {
    if (out.size() % 64 == 0) mid = clamp(mid + static_cast<Price>(rng() % 3) - 1);  // Random walk.
    Message m{};
    double r = u(rng);
    const bool thin = live.size() < cfg.target_resting;

    if (!thin && r < cfg.cancel_share) {
      m.type = MsgType::kCancel;
      m.id = live.Random(rng);
    } else if (!thin && r < cfg.cancel_share + cfg.modify_share) {
      m.type = MsgType::kModify;
      m.id = live.Random(rng);
      const LiveOrders::Info info = live.Get(m.id);
      m.side = info.side;
      if (u(rng) < 0.7 && info.qty > 1) {
        // Size reduction at the same price: keeps queue priority.
        m.price = info.price;
        m.qty = static_cast<Qty>(1 + rng() % (info.qty - 1));
        live.SetQty(m.id, m.qty);
      } else {
        // Re-price (or size up): loses priority and may trade.
        const Price step = static_cast<Price>(rng() % 3);
        m.price = clamp(info.side == Side::kBuy ? info.price + step : info.price - step);
        m.qty = lot();
        if (m.price == info.price && m.qty <= info.qty) {
          live.SetQty(m.id, m.qty);  // Turned out to be an in-place reduction.
        } else {
          live.Remove(m.id);  // Re-added by OnRest if it rests again.
        }
      }
    } else {
      const Side side = rng() & 1 ? Side::kBuy : Side::kSell;
      const Price dir = side == Side::kBuy ? -1 : 1;  // Passive side of the mid.
      m.id = next_id++;
      m.side = side;
      m.qty = lot();
      r = u(rng);
      if (r < cfg.market_share) {
        m.type = MsgType::kMarket;
      } else {
        m.type = MsgType::kLimit;
        r -= cfg.market_share;
        if (r < cfg.aggressive_share) {
          m.price = clamp(mid - dir * static_cast<Price>(rng() % 4));  // Crosses the spread.
        } else {
          m.price = clamp(mid + dir * (1 + distance(rng)));
        }
        const double t = u(rng);
        m.tif = t < cfg.ioc_share                    ? TimeInForce::kIoc
                : t < cfg.ioc_share + cfg.fok_share ? TimeInForce::kFok
                                                     : TimeInForce::kGtc;
      }
    }
    Apply(book, m);
    out.push_back(m);
  }
  return out;
}

}  // namespace ob
