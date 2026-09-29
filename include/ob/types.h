// Core types shared by the matching engine, the reference book and tools.
#pragma once

#include <cstdint>

namespace ob {

using OrderId = uint64_t;  // 0 is reserved (never a valid order id).
using Price = int64_t;     // Integer ticks: no floating point anywhere in matching.
using Qty = uint32_t;

enum class Side : uint8_t { kBuy = 0, kSell = 1 };

enum class TimeInForce : uint8_t {
  kGtc,  // Good till cancel: any unfilled remainder rests on the book.
  kIoc,  // Immediate or cancel: trade what is possible now, cancel the rest.
  kFok,  // Fill or kill: trade the full quantity immediately or do nothing.
};

enum class Reject : uint8_t {
  kInvalidId,        // Order id 0.
  kInvalidQuantity,  // Quantity 0.
  kPriceOutOfRange,  // Outside the book's configured price band.
  kDuplicateId,      // An order with this id is already resting.
  kUnknownOrder,     // Cancel/modify of an id that is not resting.
  kBookFull,         // Order pool exhausted.
  kFokNotFillable,   // Fill-or-kill could not be filled in full.
};

struct Trade {
  OrderId maker;  // Resting order.
  OrderId taker;  // Incoming order.
  Price price;    // Always the maker's price.
  Qty qty;
  Side taker_side;
};

inline constexpr Side Opposite(Side s) { return s == Side::kBuy ? Side::kSell : Side::kBuy; }

// Event sink used when an engine does not need to report anything.
struct NullListener {
  void OnTrade(const Trade&) noexcept {}
  void OnRest(OrderId, Side, Price, Qty) noexcept {}
  void OnCancel(OrderId, Qty) noexcept {}
  void OnReject(OrderId, Reject) noexcept {}
};

#if defined(__GNUC__) || defined(__clang__)
#define OB_ALWAYS_INLINE inline __attribute__((always_inline))
#define OB_NOINLINE __attribute__((noinline))
#define OB_PREFETCH(addr) __builtin_prefetch(addr)
#else
#define OB_ALWAYS_INLINE inline
#define OB_NOINLINE
#define OB_PREFETCH(addr) ((void)0)
#endif

}  // namespace ob
