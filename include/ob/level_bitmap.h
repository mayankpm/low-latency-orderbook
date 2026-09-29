// Hierarchical bitmap over price levels: bit i is set when level i has
// resting orders.
//
// Layer 0 holds one bit per level; each bit of layer k+1 says whether the
// matching 64-bit word of layer k is non-zero. Finding the next non-empty level
// above or below any index is therefore a handful of count-trailing/leading-
// zero instructions per layer: 3 layers cover 262,144 levels, 4 cover 16.7M.
// This replaces walking a std::map (pointer chasing through a red-black tree)
// when the best level empties.
#pragma once

#include <bit>
#include <cstdint>
#include <vector>

namespace ob {

class LevelBitmap {
 public:
  static constexpr uint32_t kNone = UINT32_MAX;

  explicit LevelBitmap(uint32_t size) : size_(size) {
    uint64_t bits = size;
    do {
      const uint64_t words = (bits + 63) / 64;
      layers_.emplace_back(words, 0);
      bits = words;
    } while (bits > 1);
  }

  uint32_t size() const { return size_; }

  bool Test(uint32_t i) const noexcept { return (layers_[0][i >> 6] >> (i & 63)) & 1; }

  void Set(uint32_t i) noexcept {
    uint64_t idx = i;
    for (auto& layer : layers_) {
      uint64_t& word = layer[idx >> 6];
      const bool was_empty = word == 0;
      word |= uint64_t{1} << (idx & 63);
      if (!was_empty) return;  // Upper layers already mark this word as non-empty.
      idx >>= 6;
    }
  }

  void Clear(uint32_t i) noexcept {
    uint64_t idx = i;
    for (auto& layer : layers_) {
      uint64_t& word = layer[idx >> 6];
      word &= ~(uint64_t{1} << (idx & 63));
      if (word != 0) return;  // Word still non-empty: upper layers stay set.
      idx >>= 6;
    }
  }

  // Smallest set index >= i, or kNone.
  uint32_t NextAtOrAbove(uint32_t i) const noexcept {
    if (i >= size_) return kNone;
    uint64_t idx = i;
    for (size_t l = 0; l < layers_.size(); l++) {
      const uint64_t w = idx >> 6;
      if (w < layers_[l].size()) {
        const uint64_t bits = layers_[l][w] & (~uint64_t{0} << (idx & 63));
        if (bits != 0) return Descend<true>(l, (w << 6) | static_cast<uint64_t>(std::countr_zero(bits)));
      }
      idx = w + 1;  // Continue with the next word, one layer up.
      if (idx >= uint64_t{layers_[l].size()}) return kNone;
    }
    return kNone;
  }

  // Largest set index <= i, or kNone.
  uint32_t NextAtOrBelow(uint32_t i) const noexcept {
    if (i == kNone) return kNone;
    uint64_t idx = i >= size_ ? size_ - 1 : i;
    for (size_t l = 0; l < layers_.size(); l++) {
      const uint64_t w = idx >> 6;
      const uint64_t shift = 63 - (idx & 63);
      const uint64_t bits = layers_[l][w] & (~uint64_t{0} >> shift);
      if (bits != 0) return Descend<false>(l, (w << 6) | static_cast<uint64_t>(63 - std::countl_zero(bits)));
      if (w == 0) return kNone;
      idx = w - 1;  // Continue with the previous word, one layer up.
    }
    return kNone;
  }

 private:
  // `pos` is a set bit in layer `l`; walk down to layer 0 taking the lowest
  // (or highest) set bit of each word on the way.
  template <bool kLowest>
  uint32_t Descend(size_t l, uint64_t pos) const noexcept {
    while (l > 0) {
      l--;
      const uint64_t word = layers_[l][pos];
      pos = (pos << 6) |
            static_cast<uint64_t>(kLowest ? std::countr_zero(word) : 63 - std::countl_zero(word));
    }
    return static_cast<uint32_t>(pos);
  }

  uint32_t size_;
  std::vector<std::vector<uint64_t>> layers_;  // layers_[0] is one bit per level.
};

}  // namespace ob
