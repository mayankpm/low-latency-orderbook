// Randomized checks of the building blocks against obviously-correct models.
#include <map>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>

#include "ob/id_map.h"
#include "ob/level_bitmap.h"
#include "ob/spsc_queue.h"
#include "test.h"

using namespace ob;

TEST(bitmap, next_set_bit_matches_std_set) {
  for (uint32_t size : {1u, 63u, 64u, 65u, 4096u, 4097u, 300000u}) {
    LevelBitmap bm(size);
    std::set<uint32_t> model;
    std::mt19937 rng(size);
    for (int step = 0; step < 20000; step++) {
      const uint32_t i = rng() % size;
      if (rng() % 3 == 0) {
        bm.Clear(i);
        model.erase(i);
      } else {
        bm.Set(i);
        model.insert(i);
      }
      const uint32_t q = rng() % size;
      auto up = model.lower_bound(q);
      CHECK_EQ(bm.NextAtOrAbove(q), up == model.end() ? LevelBitmap::kNone : *up);
      auto down = model.upper_bound(q);
      const uint32_t want_down = down == model.begin() ? LevelBitmap::kNone : *std::prev(down);
      CHECK_EQ(bm.NextAtOrBelow(q), want_down);
      CHECK_EQ(bm.Test(i), model.count(i) == 1);
    }
    // Boundaries.
    CHECK_EQ(bm.NextAtOrAbove(size), LevelBitmap::kNone);
  }
}

template <typename Policy>
void IdMapChurn() {
  const uint32_t kMax = 50000;
  IdMap<Policy> map(kMax);
  std::unordered_map<OrderId, uint32_t> model;
  std::mt19937_64 rng(11);
  std::vector<OrderId> keys;
  for (int step = 0; step < 500000; step++) {
    const int op = static_cast<int>(rng() % 3);
    if (op == 0 && model.size() < kMax) {
      // Mix of sequential ids (clustering risk) and random ones.
      const OrderId id = (rng() & 1) ? static_cast<OrderId>(step + 1) : (rng() | 1);
      const bool fresh = !model.count(id);
      CHECK_EQ(map.Insert(id, static_cast<uint32_t>(step)), fresh);
      if (fresh) {
        model[id] = static_cast<uint32_t>(step);
        keys.push_back(id);
      }
    } else if (op == 1 && !keys.empty()) {
      const size_t k = rng() % keys.size();
      const OrderId id = keys[k];
      keys[k] = keys.back();
      keys.pop_back();
      CHECK_EQ(map.Take(id), model.at(id));
      model.erase(id);
      CHECK_EQ(map.Take(id), IdMap<Policy>::kNone);
    } else {
      const OrderId id = keys.empty() || (rng() & 1) ? (rng() | 1) : keys[rng() % keys.size()];
      auto it = model.find(id);
      CHECK_EQ(map.Find(id), it == model.end() ? IdMap<Policy>::kNone : it->second);
    }
  }
  for (const auto& [id, v] : model) CHECK_EQ(map.Find(id), v);
}

TEST(id_map, sequential_policy_matches_unordered_map_under_churn) { IdMapChurn<SequentialIds>(); }
TEST(id_map, arbitrary_policy_matches_unordered_map_under_churn) { IdMapChurn<ArbitraryIds>(); }

TEST(spsc, transfers_every_item_in_order) {
  SpscQueue<uint64_t> q(1024);
  const uint64_t kItems = 5'000'000;
  std::thread producer([&] {
    for (uint64_t i = 1; i <= kItems; i++) {
      while (!q.TryPush(i)) {
      }
    }
  });
  uint64_t expected = 1, got = 0;
  bool in_order = true;
  while (expected <= kItems) {
    if (q.TryPop(&got)) {
      in_order = in_order && got == expected;
      expected++;
    }
  }
  producer.join();
  CHECK(in_order);
  uint64_t extra;
  CHECK(!q.TryPop(&extra));
}
