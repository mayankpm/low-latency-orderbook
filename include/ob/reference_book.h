// The textbook order book: std::map of price levels, each a std::list of
// orders, plus std::unordered_map for id lookup. Every resting order is a
// heap-allocated list node.
//
// It exists for two reasons. It is the correctness oracle: simple enough to
// trust by reading, it implements exactly the same rules and events as
// OrderBook, and the differential tests require both to produce identical
// event streams. It is also the benchmark baseline the fast book is measured
// against.
#pragma once

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ob/order_book.h"
#include "ob/types.h"

namespace ob {

template <typename Listener = NullListener>
class ReferenceBook {
 public:
  ReferenceBook(const BookConfig& cfg, Listener& listener)
      : min_price_(cfg.min_price), max_price_(cfg.max_price), max_orders_(cfg.max_orders), listener_(listener) {}

  bool AddLimit(OrderId id, Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::kGtc) {
    if (id == 0) return Rej(id, Reject::kInvalidId);
    if (qty == 0) return Rej(id, Reject::kInvalidQuantity);
    if (price < min_price_ || price > max_price_) return Rej(id, Reject::kPriceOutOfRange);
    if (index_.count(id)) return Rej(id, Reject::kDuplicateId);
    return Execute(id, side, price, qty, tif, false);
  }

  bool AddMarket(OrderId id, Side side, Qty qty) {
    if (id == 0) return Rej(id, Reject::kInvalidId);
    if (qty == 0) return Rej(id, Reject::kInvalidQuantity);
    if (index_.count(id)) return Rej(id, Reject::kDuplicateId);
    return Execute(id, side, side == Side::kBuy ? max_price_ : min_price_, qty, TimeInForce::kIoc, true);
  }

  bool Cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return Rej(id, Reject::kUnknownOrder);
    const Qty remaining = it->second->qty;
    Remove(it);
    listener_.OnCancel(id, remaining);
    return true;
  }

  bool Modify(OrderId id, Price new_price, Qty new_qty) {
    auto it = index_.find(id);
    if (it == index_.end()) return Rej(id, Reject::kUnknownOrder);
    if (new_qty == 0) return Cancel(id);
    if (new_price < min_price_ || new_price > max_price_) return Rej(id, Reject::kPriceOutOfRange);
    Order& o = *it->second;
    if (new_price == o.price && new_qty <= o.qty) {
      o.qty = new_qty;
      return true;
    }
    const Side side = o.side;
    Remove(it);
    return Execute(id, side, new_price, new_qty, TimeInForce::kGtc, false);
  }

  std::optional<Price> BestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
  }
  std::optional<Price> BestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
  }
  size_t OrderCount() const { return index_.size(); }

  using DepthLevel = typename OrderBook<Listener, SequentialIds>::DepthLevel;
  std::vector<DepthLevel> Depth(Side side, size_t n) const {
    std::vector<DepthLevel> out;
    auto collect = [&](const auto& book) {
      for (const auto& [price, queue] : book) {
        if (out.size() >= n) break;
        uint64_t total = 0;
        for (const Order& o : queue) total += o.qty;
        out.push_back({price, total, static_cast<uint32_t>(queue.size())});
      }
    };
    if (side == Side::kBuy) collect(bids_); else collect(asks_);
    return out;
  }

  std::vector<std::pair<OrderId, Qty>> QueueAt(Side side, Price price) const {
    std::vector<std::pair<OrderId, Qty>> out;
    auto collect = [&](const auto& book) {
      auto it = book.find(price);
      if (it == book.end()) return;
      for (const Order& o : it->second) out.emplace_back(o.id, o.qty);
    };
    if (side == Side::kBuy) collect(bids_); else collect(asks_);
    return out;
  }

 private:
  struct Order {
    OrderId id;
    Side side;
    Price price;
    Qty qty;
  };
  using Queue = std::list<Order>;

  bool Rej(OrderId id, Reject r) {
    listener_.OnReject(id, r);
    return false;
  }

  bool Execute(OrderId id, Side side, Price limit, Qty qty, TimeInForce tif, bool market) {
    if (tif == TimeInForce::kFok) {
      uint64_t available = 0;
      auto sum = [&](const auto& book, auto crosses) {
        for (const auto& [price, queue] : book) {
          if (!crosses(price) || available >= qty) break;
          for (const Order& o : queue) available += o.qty;
        }
      };
      if (side == Side::kBuy) sum(asks_, [&](Price p) { return p <= limit; });
      else sum(bids_, [&](Price p) { return p >= limit; });
      if (available < qty) return Rej(id, Reject::kFokNotFillable);
    }
    qty = side == Side::kBuy ? Match(asks_, id, side, qty, [&](Price p) { return p <= limit; })
                             : Match(bids_, id, side, qty, [&](Price p) { return p >= limit; });
    if (qty == 0) return true;
    if (tif != TimeInForce::kGtc || market) {
      listener_.OnCancel(id, qty);
      return true;
    }
    if (index_.size() >= max_orders_) return Rej(id, Reject::kBookFull);
    Queue& q = side == Side::kBuy ? bids_[limit] : asks_[limit];
    q.push_back(Order{id, side, limit, qty});
    index_[id] = std::prev(q.end());
    listener_.OnRest(id, side, limit, qty);
    return true;
  }

  template <typename Book, typename Crosses>
  Qty Match(Book& book, OrderId taker, Side side, Qty qty, Crosses crosses) {
    while (qty > 0 && !book.empty() && crosses(book.begin()->first)) {
      auto level = book.begin();
      Queue& q = level->second;
      while (qty > 0 && !q.empty()) {
        Order& maker = q.front();
        const Qty fill = std::min(qty, maker.qty);
        listener_.OnTrade(Trade{maker.id, taker, level->first, fill, side});
        qty -= fill;
        maker.qty -= fill;
        if (maker.qty == 0) {
          index_.erase(maker.id);
          q.pop_front();
        }
      }
      if (q.empty()) book.erase(level);
    }
    return qty;
  }

  void Remove(typename std::unordered_map<OrderId, typename Queue::iterator>::iterator it) {
    const Order o = *it->second;
    if (o.side == Side::kBuy) {
      auto level = bids_.find(o.price);
      level->second.erase(it->second);
      if (level->second.empty()) bids_.erase(level);
    } else {
      auto level = asks_.find(o.price);
      level->second.erase(it->second);
      if (level->second.empty()) asks_.erase(level);
    }
    index_.erase(it);
  }

  Price min_price_;
  Price max_price_;
  size_t max_orders_;
  Listener& listener_;
  std::map<Price, Queue, std::greater<Price>> bids_;  // Highest first.
  std::map<Price, Queue> asks_;                        // Lowest first.
  std::unordered_map<OrderId, typename Queue::iterator> index_;
};

}  // namespace ob
