# Low-Latency Order Book

A price-time priority limit order book and matching engine built for latency: 27 ns per message on average and
36 million messages per second on one core, about 4x the throughput of the textbook `std::map` + `std::list` design on
the same workload, with identical output. Header-only, no dependencies, no allocation after construction.

Supports limit (GTC), immediate-or-cancel, fill-or-kill and market orders, cancels, and modifies (reduce in place keeps
queue priority; any other change re-queues).

## Why it is fast

The common starting point is `std::map<Price, std::list<Order*>>` per side plus `std::unordered_map<Id, Order*>`.
Every one of those is a pointer-chasing, heap-allocating structure. Each is replaced here:

| Common approach | Here | Effect |
|---|---|---|
| `std::map` of price levels (red-black tree) | Flat array of levels indexed by `price - min_price` | Price to level is a subtraction; no tree walk, no rebalancing |
| Walking the map to find the next best price | Hierarchical bitmap of non-empty levels (64-ary) | Next best price in a few `tzcnt`/`lzcnt` instructions per layer |
| `std::list`/`std::deque` of orders per level | Intrusive FIFO of 32-byte nodes linked by 32-bit indices | O(1) append, fill and cancel from anywhere in the queue; 2 orders per cache line |
| `new`/`delete` per order | Preallocated node pool with a LIFO free list | No allocation on the hot path; reused nodes are still in cache |
| `std::unordered_map` (node per entry) | Open-addressing id map, linear probing, backward-shift deletion | One flat array, no tombstones, usually one cache line per lookup |
| Hashing ids randomly | Exchange-assigned ids map straight to slots (`id & mask`) | Orders that arrived together sit together in memory |
| Virtual event callbacks | Listener is a template parameter | Trade reporting inlines into the matching loop |
| Summing orders for fill-or-kill | Per-level running totals | FOK check reads one number per level |

Other details: all memory is touched in the constructor so no page faults occur while matching; matching is
templated on side so the inner loop has no side branches; the next maker node is prefetched while the current one
fills; rare paths (rejects) are kept out of line.

The id placement choice was measured rather than assumed. With the same engine and Fibonacci-hashed ids, throughput
drops from 36M to 13M messages/s once the id table is larger than the cache, because every lookup lands in a random
place. For client-chosen ids with no ordering, `OrderBook<Listener, ArbitraryIds>` switches to Fibonacci hashing (and
with a small book, where the table fits in cache, that is the faster choice).

## Correctness

* **Differential testing**: the optimized book and a simple `std::map`/`std::list` reference book process the same
  generated streams and must emit identical events (every trade, rest, cancel and reject, in order) and end with
  identical depth. 5 streams, 5 million messages and about 2 million trades: tight books with heavy trading, deep
  sparse books with heavy cancelling, a narrow price band that hits both edges, and arbitrary ids.
* **Invariant checks**: queue links, per-level totals and counts, bitmap bits, best prices, id map consistency and an
  uncrossed book, checked after every unit test and periodically during differential runs.
* **Unit tests** for each rule: price-time priority, multi-level sweeps, cancel from the middle of a queue, modify
  semantics, IOC/FOK/market, rejects, pool exhaustion, band edges.
* **Randomized structure tests**: the bitmap against `std::set`, the id map against `std::unordered_map` (both id
  policies), and the SPSC ring under a 5-million-item two-thread transfer.
* Clean under AddressSanitizer + UndefinedBehaviorSanitizer and ThreadSanitizer; builds warning-free with GCC and
  Clang. CI runs all of these on Linux and the tests on Windows.

## Benchmarks

`obbench` generates a synthetic order flow first (random-walk mid price; 73% limit orders mostly within a few ticks
of the mid, 19% cancels, 6% modifies, 2% market; about 20,000 orders resting), then replays it. Linux in Docker on an
AMD Ryzen 9 5900HS, GCC 13, `-O3 -march=native`, 10 million messages.

**Throughput, one thread**

| Book | Messages/s | ns/message |
|---|---|---|
| This book | 36.4M | 27.4 |
| Same book, Fibonacci-hashed ids | 13.4M | 74.9 |
| `std::map` + `std::list` + `std::unordered_map` reference | 8.8M | 113.4 |

Both books produce the same 5.3 million trades. On Windows (MinGW, same machine) the reference book is slower and the
gap is 5.7x (36.2M vs 6.4M).

**Latency per message** (fenced RDTSC around each call, timer overhead subtracted), nanoseconds:

| Message | p50 | p90 | p99 | p99.9 | Reference p50 / p99 / p99.9 |
|---|---|---|---|---|---|
| All | 30 | 80 | 200 | 711 | 90 / 421 / 3,447 |
| Limit that rests | 20 | 30 | 50 | 140 | 70 / 240 / 621 |
| Limit that trades | 40 | 90 | 190 | 872 | 100 / 822 / 8,056 |
| Cancel | 60 | 140 | 331 | 1,232 | 150 / 471 / 1,994 |
| Modify | 60 | 140 | 331 | 1,232 | 80 / 461 / 1,773 |
| Market | 50 | 110 | 210 | 681 | 110 / 451 / 1,533 |

Cancels cost the most because they touch an arbitrary old order, which is usually no longer in cache.

**Two-thread pipeline** (gateway thread to matching thread through the SPSC ring, threads pinned):

* 16.2M messages/s end to end.
* At a steady 1M messages/s, enqueue to processed takes 190 ns at the median. Tail latencies here (p99 around 100 us)
  come from the Docker VM's virtual CPUs being scheduled by the host; on dedicated, isolated cores they would be far
  lower.

## Usage

```cpp
#include "ob/order_book.h"

struct Printer {
  void OnTrade(const ob::Trade& t) { std::cout << "trade " << t.taker << " x " << t.qty << " @ " << t.price << "\n"; }
  void OnRest(ob::OrderId, ob::Side, ob::Price, ob::Qty) {}
  void OnCancel(ob::OrderId, ob::Qty) {}
  void OnReject(ob::OrderId, ob::Reject) {}
};

Printer events;
ob::OrderBook<Printer> book(ob::BookConfig{/*min_price=*/1, /*max_price=*/100000, /*max_orders=*/1 << 20}, events);
book.AddLimit(1, ob::Side::kSell, 10050, 100);
book.AddLimit(2, ob::Side::kBuy, 10050, 40);                 // trades 40 @ 10050
book.AddLimit(3, ob::Side::kBuy, 10060, 100, ob::TimeInForce::kIoc);
book.Modify(1, 10050, 30);                                   // reduce, keeps priority
book.Cancel(1);
```

Prices are integer ticks within a configured band (like an exchange's price limits); orders outside it are
rejected. Order id 0 is reserved.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOB_NATIVE=ON
cmake --build build -j
./build/obtests
./build/obbench --messages 10000000 --cpu 2 --cpu2 4
cmake -S . -B build-tsan -DOB_SANITIZE=thread && cmake --build build-tsan && ./build-tsan/obtests
```

## Layout

```
include/ob/order_book.h      the matching engine
include/ob/level_bitmap.h    hierarchical bitmap of non-empty price levels
include/ob/id_map.h          open-addressing order id map with id placement policies
include/ob/spsc_queue.h      lock-free single-producer/single-consumer ring
include/ob/reference_book.h  std::map/std::list book: test oracle and benchmark baseline
include/ob/workload.h        synthetic order flow generator
include/ob/timing.h          RDTSC timing and thread pinning
apps/obbench.cpp             benchmarks
tests/                       unit, randomized and differential tests
```

## Limitations

* One instrument per book and one thread per book, as in exchange matching engines that shard by symbol.
* The price band must be set up front; the ladder uses 24 bytes per tick per side (2.4 MB per side for 100,000 ticks).
* No self-trade prevention, iceberg or stop orders, auctions, or market data feed output beyond the event callbacks.
