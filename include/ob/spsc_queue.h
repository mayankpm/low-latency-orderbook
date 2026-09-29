// Bounded lock-free single-producer/single-consumer ring buffer, for handing
// orders from a gateway thread to the matching thread.
//
//  * Head and tail live on separate cache lines, so the two threads never
//    write to the same line (no false sharing).
//  * Each side keeps a cached copy of the other side's index and only
//    re-reads the shared atomic when the cache says the ring looks full or
//    empty. In steady state a push or pop touches no line owned by the
//    other thread except the slot itself.
//  * Capacity is a power of two so wrapping is a mask, not a division.
#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <vector>

namespace ob {

inline constexpr size_t kCacheLine = 64;

template <typename T>
class SpscQueue {
 public:
  explicit SpscQueue(size_t capacity) : slots_(std::bit_ceil(capacity < 2 ? 2 : capacity)), mask_(slots_.size() - 1) {}

  // Producer thread only. Returns false if full.
  bool TryPush(const T& value) noexcept {
    const size_t tail = tail_.value.load(std::memory_order_relaxed);
    if (tail - head_cache_ == slots_.size()) {
      head_cache_ = head_.value.load(std::memory_order_acquire);
      if (tail - head_cache_ == slots_.size()) return false;
    }
    slots_[tail & mask_] = value;
    tail_.value.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only. Returns false if empty.
  bool TryPop(T* out) noexcept {
    const size_t head = head_.value.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.value.load(std::memory_order_acquire);
      if (head == tail_cache_) return false;
    }
    *out = slots_[head & mask_];
    head_.value.store(head + 1, std::memory_order_release);
    return true;
  }

  size_t capacity() const { return slots_.size(); }

 private:
  struct alignas(kCacheLine) PaddedIndex {
    std::atomic<size_t> value{0};
  };

  std::vector<T> slots_;
  const size_t mask_;
  PaddedIndex head_;                            // Written by the consumer.
  alignas(kCacheLine) size_t tail_cache_ = 0;   // Consumer's view of tail.
  PaddedIndex tail_;                            // Written by the producer.
  alignas(kCacheLine) size_t head_cache_ = 0;   // Producer's view of head.
};

}  // namespace ob
