// Low-latency limit order book with price-time priority matching.
//
// Layout, chosen for the hot path:
//  * Price ladder: one Level per tick in the configured band, in a flat array
//    per side, so a price maps to its level by subtraction (no tree, no hash).
//  * LevelBitmap per side finds the next non-empty level when the best level
//    empties, in a few bit-scan instructions.
//  * Orders are 32-byte nodes in one preallocated pool, linked into per-level
//    FIFO queues by 32-bit indices (intrusive doubly linked list). Appending,
//    filling from the front and cancelling from the middle are all O(1), and
//    two nodes fit in a cache line. Freed nodes are reused LIFO, so the next
//    order lands in memory that is still in cache.
//  * IdMap (open addressing) finds an order's node for cancel/modify.
//  * All memory is allocated and touched in the constructor; processing an
//    order never allocates, never frees and never takes a lock.
//  * The event sink is a template parameter, so trade reporting inlines
//    instead of going through a virtual call.
//
// One book is single threaded by design (exchanges shard matching by
// instrument); see spsc_queue.h for handing orders in from another thread.
#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ob/id_map.h"
#include "ob/level_bitmap.h"
#include "ob/types.h"

namespace ob {

struct BookConfig {
  Price min_price = 1;           // Lowest accepted price, in ticks.
  Price max_price = 1 << 16;     // Highest accepted price (a price band, like exchange limit bands).
  uint32_t max_orders = 1 << 20; // Resting order capacity.
};

template <typename Listener = NullListener, typename IdPolicy = SequentialIds>
class OrderBook {
 public:
  static constexpr uint32_t kNil = UINT32_MAX;
  static constexpr uint32_t kNone = IdMap<IdPolicy>::kNone;

  OrderBook(const BookConfig& cfg, Listener& listener)
      : min_price_(cfg.min_price),
        num_levels_(static_cast<uint32_t>(cfg.max_price - cfg.min_price + 1)),
        listener_(listener),
        ids_(cfg.max_orders),
        bitmaps_{LevelBitmap(num_levels_), LevelBitmap(num_levels_)} {
    levels_[0].assign(num_levels_, Level{});
    levels_[1].assign(num_levels_, Level{});
    orders_.assign(cfg.max_orders, Order{});
    // Thread the free list through the pool (lowest index first).
    for (uint32_t i = 0; i < cfg.max_orders; i++) orders_[i].next = i + 1 < cfg.max_orders ? i + 1 : kNil;
    free_head_ = cfg.max_orders > 0 ? 0 : kNil;
  }

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  // Limit order. Trades against the opposite side while prices cross, then
  // handles any remainder according to `tif`.
  bool AddLimit(OrderId id, Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::kGtc) noexcept {
    if (!Validate(id, price, qty)) [[unlikely]] return false;
    const uint32_t level = static_cast<uint32_t>(price - min_price_);
    return side == Side::kBuy ? Execute<Side::kBuy>(id, level, qty, tif, /*market=*/false)
                              : Execute<Side::kSell>(id, level, qty, tif, /*market=*/false);
  }

  // Market order: trades at any price until filled or the opposite side is
  // empty; the remainder is cancelled.
  bool AddMarket(OrderId id, Side side, Qty qty) noexcept {
    if (id == 0 || qty == 0) [[unlikely]] return RejectOrder(id, id == 0 ? Reject::kInvalidId : Reject::kInvalidQuantity);
    if (ids_.Find(id) != kNone) [[unlikely]] return RejectOrder(id, Reject::kDuplicateId);
    return side == Side::kBuy ? Execute<Side::kBuy>(id, num_levels_ - 1, qty, TimeInForce::kIoc, true)
                              : Execute<Side::kSell>(id, 0, qty, TimeInForce::kIoc, true);
  }

  bool Cancel(OrderId id) noexcept {
    const uint32_t idx = ids_.Take(id);
    if (idx == kNone) [[unlikely]] return RejectOrder(id, Reject::kUnknownOrder);
    const Qty remaining = orders_[idx].qty;
    Unlink(idx);
    FreeNode(idx);
    listener_.OnCancel(id, remaining);
    return true;
  }

  // Reducing quantity at the same price keeps the order's place in the
  // queue. Any other change (new price, larger quantity) loses priority: the
  // order is removed and re-entered, and may trade immediately. Quantity 0
  // cancels.
  bool Modify(OrderId id, Price new_price, Qty new_qty) noexcept {
    const uint32_t idx = ids_.Find(id);
    if (idx == kNone) [[unlikely]] return RejectOrder(id, Reject::kUnknownOrder);
    if (new_qty == 0) return Cancel(id);
    if (new_price < min_price_ || new_price > min_price_ + num_levels_ - 1) [[unlikely]] {
      return RejectOrder(id, Reject::kPriceOutOfRange);
    }
    Order& o = orders_[idx];
    const uint32_t new_level = static_cast<uint32_t>(new_price - min_price_);
    if (new_level == o.level && new_qty <= o.qty) [[likely]] {
      levels_[static_cast<int>(o.side)][o.level].total -= o.qty - new_qty;
      o.qty = new_qty;
      return true;
    }
    const Side side = o.side;
    ids_.Take(id);
    Unlink(idx);
    FreeNode(idx);
    return side == Side::kBuy ? Execute<Side::kBuy>(id, new_level, new_qty, TimeInForce::kGtc, false)
                              : Execute<Side::kSell>(id, new_level, new_qty, TimeInForce::kGtc, false);
  }

  // ---- queries (not on the matching hot path) -------------------------------

  std::optional<Price> BestBid() const { return BestPrice(Side::kBuy); }
  std::optional<Price> BestAsk() const { return BestPrice(Side::kSell); }

  uint64_t VolumeAt(Side side, Price price) const {
    if (price < min_price_ || price > min_price_ + num_levels_ - 1) return 0;
    return levels_[static_cast<int>(side)][price - min_price_].total;
  }

  size_t OrderCount() const { return live_orders_; }

  struct DepthLevel {
    Price price;
    uint64_t qty;
    uint32_t orders;
    bool operator==(const DepthLevel&) const = default;
  };

  // Best `n` levels of one side, best first.
  std::vector<DepthLevel> Depth(Side side, size_t n) const {
    std::vector<DepthLevel> out;
    const int s = static_cast<int>(side);
    uint32_t lvl = best_[s];
    while (lvl != kNil && out.size() < n) {
      const Level& l = levels_[s][lvl];
      out.push_back({min_price_ + lvl, l.total, l.count});
      if (side == Side::kBuy) {
        lvl = lvl == 0 ? kNil : bitmaps_[s].NextAtOrBelow(lvl - 1);
      } else {
        lvl = bitmaps_[s].NextAtOrAbove(lvl + 1);
      }
    }
    return out;
  }

  // Resting orders at one price, oldest first.
  std::vector<std::pair<OrderId, Qty>> QueueAt(Side side, Price price) const {
    std::vector<std::pair<OrderId, Qty>> out;
    if (price < min_price_ || price > min_price_ + num_levels_ - 1) return out;
    for (uint32_t i = levels_[static_cast<int>(side)][price - min_price_].head; i != kNil; i = orders_[i].next) {
      out.emplace_back(orders_[i].id, orders_[i].qty);
    }
    return out;
  }

  // Full consistency check for tests: queue links, per-level totals and
  // counts, bitmap bits, best prices, id map, and that the book is not crossed.
  bool CheckInvariants(std::string* error) const {
    size_t orders = 0;
    for (int s = 0; s < 2; s++) {
      uint32_t best = kNil;
      for (uint32_t lvl = 0; lvl < num_levels_; lvl++) {
        const Level& l = levels_[s][lvl];
        uint64_t total = 0;
        uint32_t count = 0;
        uint32_t prev = kNil;
        for (uint32_t i = l.head; i != kNil; i = orders_[i].next) {
          const Order& o = orders_[i];
          if (o.prev != prev || o.level != lvl || static_cast<int>(o.side) != s || o.qty == 0 ||
              ids_.Find(o.id) != i) {
            *error = "bad order node at level " + std::to_string(lvl);
            return false;
          }
          total += o.qty;
          count++;
          prev = i;
        }
        if (prev != l.tail || total != l.total || count != l.count || bitmaps_[s].Test(lvl) != (count > 0)) {
          *error = "bad level " + std::to_string(lvl) + " side " + std::to_string(s);
          return false;
        }
        if (count > 0 && (best == kNil || (s == 0 ? lvl > best : lvl < best))) best = lvl;
        orders += count;
      }
      if (best != best_[s]) {
        *error = "stale best level on side " + std::to_string(s);
        return false;
      }
    }
    if (orders != live_orders_) {
      *error = "order count mismatch";
      return false;
    }
    if (best_[0] != kNil && best_[1] != kNil && best_[0] >= best_[1]) {
      *error = "book is crossed";
      return false;
    }
    return true;
  }

 private:
  struct Order {  // 32 bytes: two per cache line.
    OrderId id = 0;
    Qty qty = 0;           // Remaining quantity.
    uint32_t level = 0;    // Ladder index.
    uint32_t prev = kNil;  // Neighbours in the level's FIFO queue.
    uint32_t next = kNil;
    Side side = Side::kBuy;
  };
  static_assert(sizeof(Order) == 32, "Order node should stay at 32 bytes");

  struct Level {
    uint32_t head = kNil;  // Oldest order (first to fill).
    uint32_t tail = kNil;  // Newest order.
    uint32_t count = 0;
    uint64_t total = 0;    // Sum of remaining quantity.
  };

  bool Validate(OrderId id, Price price, Qty qty) noexcept {
    if (id == 0) return RejectOrder(id, Reject::kInvalidId);
    if (qty == 0) return RejectOrder(id, Reject::kInvalidQuantity);
    if (price < min_price_ || price > min_price_ + num_levels_ - 1) return RejectOrder(id, Reject::kPriceOutOfRange);
    if (ids_.Find(id) != kNone) return RejectOrder(id, Reject::kDuplicateId);
    return true;
  }

  OB_NOINLINE bool RejectOrder(OrderId id, Reject reason) noexcept {
    listener_.OnReject(id, reason);
    return false;
  }

  std::optional<Price> BestPrice(Side side) const {
    const uint32_t b = best_[static_cast<int>(side)];
    if (b == kNil) return std::nullopt;
    return min_price_ + b;
  }

  // Does a taker on side S with limit `limit` cross resting level `level`?
  template <Side S>
  static OB_ALWAYS_INLINE bool Crosses(uint32_t level, uint32_t limit) noexcept {
    if constexpr (S == Side::kBuy) {
      return level <= limit;
    } else {
      return level >= limit;
    }
  }

  // Quantity available against a taker on side S up to `limit`, stopping as
  // soon as `needed` is reached. Uses per-level totals, not individual orders.
  template <Side S>
  bool CanFill(uint32_t limit, Qty needed) const noexcept {
    constexpr int opp = S == Side::kBuy ? 1 : 0;
    uint64_t available = 0;
    uint32_t lvl = best_[opp];
    while (lvl != kNil && Crosses<S>(lvl, limit)) {
      available += levels_[opp][lvl].total;
      if (available >= needed) return true;
      if constexpr (S == Side::kBuy) {
        lvl = bitmaps_[opp].NextAtOrAbove(lvl + 1);
      } else {
        lvl = lvl == 0 ? kNil : bitmaps_[opp].NextAtOrBelow(lvl - 1);
      }
    }
    return false;
  }

  template <Side S>
  OB_ALWAYS_INLINE bool Execute(OrderId id, uint32_t limit, Qty qty, TimeInForce tif, bool market) noexcept {
    if (tif == TimeInForce::kFok && !CanFill<S>(limit, qty)) [[unlikely]] {
      return RejectOrder(id, Reject::kFokNotFillable);
    }
    const Qty left = Match<S>(id, limit, qty);
    if (left == 0) return true;
    if (tif != TimeInForce::kGtc || market) {
      listener_.OnCancel(id, left);  // IOC/market remainder never rests.
      return true;
    }
    return Rest<S>(id, limit, left);
  }

  // Fills a taker on side S against the opposite side, best price first and
  // oldest order first within a price. Returns the unfilled quantity.
  template <Side S>
  OB_ALWAYS_INLINE Qty Match(OrderId taker, uint32_t limit, Qty qty) noexcept {
    constexpr int opp = S == Side::kBuy ? 1 : 0;
    std::vector<Level>& levels = levels_[opp];
    uint32_t best = best_[opp];
    while (qty != 0 && best != kNil && Crosses<S>(best, limit)) {
      Level& lvl = levels[best];
      const Price price = min_price_ + best;
      uint32_t idx = lvl.head;
      while (idx != kNil) {
        Order& maker = orders_[idx];
        const uint32_t next = maker.next;
        if (next != kNil) OB_PREFETCH(&orders_[next]);  // Next maker is likely needed.
        const Qty fill = maker.qty < qty ? maker.qty : qty;
        listener_.OnTrade(Trade{maker.id, taker, price, fill, S});
        qty -= fill;
        maker.qty -= fill;
        lvl.total -= fill;
        if (maker.qty != 0) break;  // Maker partly filled, so the taker is done.
        ids_.Take(maker.id);
        lvl.count--;
        FreeNode(idx);
        idx = next;
        if (qty == 0) break;
      }
      lvl.head = idx;
      if (idx != kNil) {
        orders_[idx].prev = kNil;
        break;  // Level still has orders, so the taker is exhausted.
      }
      lvl.tail = kNil;
      bitmaps_[opp].Clear(best);
      if constexpr (S == Side::kBuy) {
        best = bitmaps_[opp].NextAtOrAbove(best);
      } else {
        best = bitmaps_[opp].NextAtOrBelow(best);
      }
    }
    best_[opp] = best;
    return qty;
  }

  template <Side S>
  OB_ALWAYS_INLINE bool Rest(OrderId id, uint32_t level, Qty qty) noexcept {
    constexpr int s = static_cast<int>(S);
    const uint32_t idx = free_head_;
    if (idx == kNil) [[unlikely]] return RejectOrder(id, Reject::kBookFull);
    free_head_ = orders_[idx].next;
    live_orders_++;

    Level& lvl = levels_[s][level];
    Order& o = orders_[idx];
    o.id = id;
    o.qty = qty;
    o.level = level;
    o.side = S;
    o.next = kNil;
    o.prev = lvl.tail;
    if (lvl.tail != kNil) {
      orders_[lvl.tail].next = idx;
    } else {
      lvl.head = idx;
      bitmaps_[s].Set(level);
      const uint32_t best = best_[s];
      if constexpr (S == Side::kBuy) {
        if (best == kNil || level > best) best_[s] = level;
      } else {
        if (best == kNil || level < best) best_[s] = level;
      }
    }
    lvl.tail = idx;
    lvl.count++;
    lvl.total += qty;
    ids_.Insert(id, idx);
    listener_.OnRest(id, S, min_price_ + level, qty);
    return true;
  }

  // Removes a resting order from its level (the id map is handled by the caller).
  void Unlink(uint32_t idx) noexcept {
    Order& o = orders_[idx];
    const int s = static_cast<int>(o.side);
    Level& lvl = levels_[s][o.level];
    if (o.prev != kNil) orders_[o.prev].next = o.next; else lvl.head = o.next;
    if (o.next != kNil) orders_[o.next].prev = o.prev; else lvl.tail = o.prev;
    lvl.count--;
    lvl.total -= o.qty;
    if (lvl.count == 0) {
      bitmaps_[s].Clear(o.level);
      if (best_[s] == o.level) {
        best_[s] = o.side == Side::kBuy ? (o.level == 0 ? kNil : bitmaps_[s].NextAtOrBelow(o.level - 1))
                                        : bitmaps_[s].NextAtOrAbove(o.level + 1);
      }
    }
  }

  void FreeNode(uint32_t idx) noexcept {
    orders_[idx].next = free_head_;
    free_head_ = idx;
    live_orders_--;
  }

  const Price min_price_;
  const uint32_t num_levels_;
  Listener& listener_;
  IdMap<IdPolicy> ids_;
  LevelBitmap bitmaps_[2];
  std::vector<Level> levels_[2];
  std::vector<Order> orders_;
  uint32_t free_head_ = kNil;
  uint32_t best_[2] = {kNil, kNil};  // Ladder index of best bid / best ask.
  size_t live_orders_ = 0;
};

}  // namespace ob
