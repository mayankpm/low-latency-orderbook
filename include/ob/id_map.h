// Order id -> pool slot map: open addressing with linear probing.
//
// Compared with std::unordered_map (one heap node per entry, a pointer chase
// per lookup), every entry lives inline in one flat array allocated up front,
// so the hot path never allocates and a lookup is usually a single cache
// line. Deletion uses backward shifting instead of tombstones, so probe
// lengths stay short no matter how many cancels the book has seen.
//
// Where an id lands is a policy:
//  * SequentialIds (default): exchange-assigned ids are dense and increasing,
//    so slot = id & mask. Orders that arrived close in time sit next to each
//    other, and the hot part of the table is a small contiguous window.
//  * ArbitraryIds: Fibonacci hashing for client-chosen ids with no pattern.
//    It spreads keys over the whole table, which defends against clustering
//    but costs a cache miss per lookup once the table outgrows the cache
//    (measured: 2.5x lower throughput on the benchmark workload).
#pragma once

#include <bit>
#include <cstdint>
#include <vector>

#include "ob/types.h"

namespace ob {

struct SequentialIds {
  static uint64_t Home(OrderId id, uint64_t mask, int /*shift*/) noexcept { return id & mask; }
};

struct ArbitraryIds {
  static uint64_t Home(OrderId id, uint64_t /*mask*/, int shift) noexcept {
    return (id * 0x9E3779B97F4A7C15ULL) >> shift;
  }
};

template <typename Policy = SequentialIds>
class IdMap {
 public:
  static constexpr uint32_t kNone = UINT32_MAX;

  // Sized for at most `max_entries` live keys at a load factor <= 0.5.
  explicit IdMap(uint32_t max_entries) {
    const uint64_t capacity = std::bit_ceil(uint64_t{max_entries} * 2 < 16 ? 16 : uint64_t{max_entries} * 2);
    slots_.assign(capacity, Slot{0, 0});
    mask_ = capacity - 1;
    shift_ = 64 - std::countr_zero(capacity);
  }

  uint32_t Find(OrderId id) const noexcept {
    for (uint64_t i = Home(id);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.key == id) return s.value;
      if (s.key == 0) return kNone;
    }
  }

  // Returns false (and changes nothing) if `id` is already present.
  bool Insert(OrderId id, uint32_t value) noexcept {
    for (uint64_t i = Home(id);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (s.key == id) return false;
      if (s.key == 0) {
        s.key = id;
        s.value = value;
        return true;
      }
    }
  }

  // Removes `id` and returns its value, or kNone. One probe sequence for
  // both the lookup and the deletion.
  uint32_t Take(OrderId id) noexcept {
    uint64_t i = Home(id);
    for (;; i = (i + 1) & mask_) {
      if (slots_[i].key == id) break;
      if (slots_[i].key == 0) return kNone;
    }
    const uint32_t value = slots_[i].value;
    // Backward-shift deletion: pull later entries of the cluster into the
    // hole when their home slot allows it, so no tombstones are needed.
    uint64_t j = i;
    while (true) {
      j = (j + 1) & mask_;
      if (slots_[j].key == 0) break;
      const uint64_t home = Home(slots_[j].key);
      // Entry j may stay put if its home lies cyclically in (i, j].
      const bool stays = i <= j ? (i < home && home <= j) : (i < home || home <= j);
      if (stays) continue;
      slots_[i] = slots_[j];
      i = j;
    }
    slots_[i].key = 0;
    return value;
  }

  void Update(OrderId id, uint32_t value) noexcept {
    for (uint64_t i = Home(id);; i = (i + 1) & mask_) {
      if (slots_[i].key == id) {
        slots_[i].value = value;
        return;
      }
      if (slots_[i].key == 0) return;
    }
  }

  // Hint the cache about the slot `id` will probe first.
  void Prefetch(OrderId id) const noexcept { OB_PREFETCH(&slots_[Home(id)]); }

 private:
  struct Slot {
    OrderId key;  // 0 = empty.
    uint32_t value;
  };

  uint64_t Home(OrderId id) const noexcept { return Policy::Home(id, mask_, shift_); }

  std::vector<Slot> slots_;
  uint64_t mask_ = 0;
  int shift_ = 0;
};

}  // namespace ob
