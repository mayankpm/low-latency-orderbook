// obbench: throughput and latency of the order book.
//
//   obbench [--messages N] [--seed S] [--cpu A] [--cpu2 B]
//
// 1. Throughput: the same pre-generated message stream replayed through the
//    optimized OrderBook and the std::map/std::list ReferenceBook.
// 2. Latency: every message timed individually with fenced RDTSC and broken
//    down by message type (timer overhead is measured and subtracted).
// 3. Pipeline: a gateway thread feeds the matching thread through the SPSC
//    ring; end-to-end latency from enqueue to "order processed", measured at
//    a paced rate so it reflects handoff cost rather than queueing.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "ob/order_book.h"
#include "ob/reference_book.h"
#include "ob/spsc_queue.h"
#include "ob/timing.h"
#include "ob/workload.h"

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

struct Counting {
  uint64_t trades = 0;
  uint64_t volume = 0;
  void OnTrade(const Trade& t) noexcept {
    trades++;
    volume += t.qty;
  }
  void OnRest(OrderId, Side, Price, Qty) noexcept {}
  void OnCancel(OrderId, Qty) noexcept {}
  void OnReject(OrderId, Reject) noexcept {}
};

BookConfig kBook{1, 100'000, 1 << 20};

// Single-parameter aliases, so the books can be passed as template template
// arguments on compilers without relaxed template-template matching (clang 18).
template <typename L>
using FastBook = OrderBook<L, SequentialIds>;
template <typename L>
using ArbitraryIdBook = OrderBook<L, ArbitraryIds>;

template <template <typename> class Book>
double ReplaySeconds(const std::vector<Message>& msgs, uint64_t* trades) {
  Counting c;
  auto book = std::make_unique<Book<Counting>>(kBook, c);
  const auto t0 = Clock::now();
  for (const Message& m : msgs) Apply(*book, m);
  const double s = std::chrono::duration<double>(Clock::now() - t0).count();
  *trades = c.trades;
  return s;
}

template <template <typename> class Book>
double BestRate(const std::vector<Message>& msgs, int runs, uint64_t* trades) {
  double best = 1e18;
  for (int r = 0; r < runs; r++) best = std::min(best, ReplaySeconds<Book>(msgs, trades));
  return static_cast<double>(msgs.size()) / best;
}

struct Percentiles {
  double p50, p90, p99, p999, max;
  size_t n;
};

Percentiles Summarize(std::vector<uint64_t>& ticks, double overhead) {
  Percentiles p{};
  p.n = ticks.size();
  if (ticks.empty()) return p;
  std::sort(ticks.begin(), ticks.end());
  const double tpn = TicksPerNs();
  auto at = [&](double q) {
    const double t = static_cast<double>(ticks[static_cast<size_t>(q * static_cast<double>(ticks.size() - 1))]);
    return std::max(0.0, t - overhead) / tpn;
  };
  p.p50 = at(0.50);
  p.p90 = at(0.90);
  p.p99 = at(0.99);
  p.p999 = at(0.999);
  p.max = at(1.0);
  return p;
}

void PrintRow(const char* label, const Percentiles& p) {
  if (p.n == 0) return;
  std::printf("  %-22s %9zu   %7.0f %7.0f %7.0f %8.0f %9.0f\n", label, p.n, p.p50, p.p90, p.p99, p.p999, p.max);
}

// Median cost of an empty timed region, subtracted from every sample.
double TimerOverhead() {
  std::vector<uint64_t> t(100000);
  for (auto& x : t) {
    const uint64_t a = ReadTicks();
    const uint64_t b = ReadTicks();
    x = b - a;
  }
  std::sort(t.begin(), t.end());
  return static_cast<double>(t[t.size() / 2]);
}

template <template <typename> class Book>
void LatencyBreakdown(const char* title, const std::vector<Message>& msgs, double overhead) {
  Counting c;
  auto book = std::make_unique<Book<Counting>>(kBook, c);
  // Categories: passive add, aggressive add (traded), cancel, modify, market.
  std::vector<uint64_t> cat[5], all;
  for (auto& v : cat) v.reserve(msgs.size() / 2);
  all.reserve(msgs.size());
  for (const Message& m : msgs) {
    const uint64_t trades_before = c.trades;
    const uint64_t t0 = ReadTicks();
    Apply(*book, m);
    const uint64_t t1 = ReadTicks();
    const uint64_t d = t1 - t0;
    all.push_back(d);
    switch (m.type) {
      case MsgType::kLimit: cat[c.trades != trades_before ? 1 : 0].push_back(d); break;
      case MsgType::kCancel: cat[2].push_back(d); break;
      case MsgType::kModify: cat[3].push_back(d); break;
      case MsgType::kMarket: cat[4].push_back(d); break;
    }
  }
  std::printf("\n%s: per-message latency (ns)\n", title);
  std::printf("  %-22s %9s   %7s %7s %7s %8s %9s\n", "message", "count", "p50", "p90", "p99", "p99.9", "max");
  PrintRow("all", Summarize(all, overhead));
  PrintRow("limit, rests", Summarize(cat[0], overhead));
  PrintRow("limit, trades", Summarize(cat[1], overhead));
  PrintRow("cancel", Summarize(cat[2], overhead));
  PrintRow("modify", Summarize(cat[3], overhead));
  PrintRow("market", Summarize(cat[4], overhead));
}

struct Timed {
  Message msg;
  uint64_t sent;
};

// Pure handoff throughput: no timestamps or per-message bookkeeping, so this
// measures the SPSC ring plus matching, not the instrumentation.
void PipelineThroughput(const std::vector<Message>& msgs, int cpu_gateway, int cpu_engine) {
  SpscQueue<Message> queue(1 << 16);
  Counting c;
  auto book = std::make_unique<OrderBook<Counting>>(kBook, c);
  std::atomic<bool> ready{false};
  std::thread engine([&] {
    PinThisThread(cpu_engine);
    ready = true;
    Message m;
    for (size_t done = 0; done < msgs.size();) {
      if (queue.TryPop(&m)) {
        Apply(*book, m);
        done++;
      }
    }
  });
  PinThisThread(cpu_gateway);
  while (!ready) {
  }
  const auto t0 = Clock::now();
  for (const Message& m : msgs) {
    while (!queue.TryPush(m)) {
    }
  }
  engine.join();
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  std::printf("  unpaced, uninstrumented: %5.1fM msg/s through the ring and the book\n",
              static_cast<double>(msgs.size()) / secs / 1e6);
}

void Pipeline(const std::vector<Message>& msgs, int cpu_gateway, int cpu_engine, double rate_per_sec) {
  SpscQueue<Timed> queue(1 << 16);
  Counting c;
  auto book = std::make_unique<OrderBook<Counting>>(kBook, c);
  std::vector<uint64_t> latency;
  latency.reserve(msgs.size());
  std::atomic<bool> ready{false};

  std::thread engine([&] {
    PinThisThread(cpu_engine);
    ready = true;
    Timed t;
    size_t done = 0;
    while (done < msgs.size()) {
      if (!queue.TryPop(&t)) continue;  // Busy-poll: no sleeping on the hot path.
      Apply(*book, t.msg);
      latency.push_back(ReadTicks() - t.sent);
      done++;
    }
  });
  PinThisThread(cpu_gateway);
  while (!ready) {
  }
  const double ticks_per_msg = rate_per_sec > 0 ? TicksPerNs() * 1e9 / rate_per_sec : 0;
  const auto t0 = Clock::now();
  uint64_t next = ReadTicks();
  for (const Message& m : msgs) {
    if (ticks_per_msg > 0) {
      while (ReadTicks() < next) {
      }
      next += static_cast<uint64_t>(ticks_per_msg);
    }
    Timed t{m, ReadTicks()};
    while (!queue.TryPush(t)) {
    }
  }
  engine.join();
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  std::sort(latency.begin(), latency.end());
  const double tpn = TicksPerNs();
  auto at = [&](double q) {
    return static_cast<double>(latency[static_cast<size_t>(q * static_cast<double>(latency.size() - 1))]) / tpn;
  };
  if (rate_per_sec > 0) {
    std::printf("  paced at %4.1fM msg/s: enqueue -> processed  p50 %5.0f ns  p99 %6.0f ns  p99.9 %7.0f ns\n",
                rate_per_sec / 1e6, at(0.5), at(0.99), at(0.999));
  } else {
    std::printf("  unpaced (max rate):  %6.1fM msg/s end to end across two threads\n",
                static_cast<double>(msgs.size()) / secs / 1e6);
  }
}

}  // namespace

int main(int argc, char** argv) {
  size_t messages = 10'000'000;
  uint64_t seed = 42;
  int cpu_a = 2, cpu_b = 4;
  bool throughput_only = false;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string f = argv[i];
    if (f == "--messages") messages = std::strtoull(argv[i + 1], nullptr, 10);
    else if (f == "--seed") seed = std::strtoull(argv[i + 1], nullptr, 10);
    else if (f == "--cpu") cpu_a = std::atoi(argv[i + 1]);
    else if (f == "--cpu2") cpu_b = std::atoi(argv[i + 1]);
    else if (f == "--max-orders") kBook.max_orders = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
    else if (f == "--mode") throughput_only = std::string(argv[i + 1]) == "throughput";
  }

  WorkloadConfig wc;
  wc.seed = seed;
  wc.messages = messages;
  const auto t0 = Clock::now();
  const auto msgs = GenerateWorkload(wc, kBook);
  size_t counts[4] = {0, 0, 0, 0};
  for (const Message& m : msgs) counts[static_cast<int>(m.type)]++;
  std::printf("workload: %zu messages (%.0f%% limit, %.0f%% cancel, %.0f%% modify, %.0f%% market), generated in %.1fs\n",
              msgs.size(), 100.0 * counts[0] / msgs.size(), 100.0 * counts[2] / msgs.size(),
              100.0 * counts[3] / msgs.size(), 100.0 * counts[1] / msgs.size(),
              std::chrono::duration<double>(Clock::now() - t0).count());
  std::printf("timer: %.2f ticks/ns\n", TicksPerNs());

  PinThisThread(cpu_a);
  uint64_t trades_fast = 0, trades_ref = 0;
  const double fast = BestRate<FastBook>(msgs, 3, &trades_fast);
  const double ref = BestRate<ReferenceBook>(msgs, 2, &trades_ref);
  uint64_t trades_arb = 0;
  const double arb = BestRate<ArbitraryIdBook>(msgs, 3, &trades_arb);
  std::printf("\nthroughput (best of runs, one thread)\n");
  std::printf("  optimized OrderBook     %8.1fM msg/s   (%.1f ns/msg)\n", fast / 1e6, 1e9 / fast);
  std::printf("  same, Fibonacci-hashed ids %5.1fM msg/s   (%.1f ns/msg)\n", arb / 1e6, 1e9 / arb);
  std::printf("  std::map/list reference %8.1fM msg/s   (%.1f ns/msg)\n", ref / 1e6, 1e9 / ref);
  std::printf("  speedup                 %8.1fx          trades: %llu (identical: %s)\n", fast / ref,
              static_cast<unsigned long long>(trades_fast), trades_fast == trades_ref ? "yes" : "NO");

  if (throughput_only) return trades_fast == trades_ref ? 0 : 1;
  const double overhead = TimerOverhead();
  std::printf("\ntimer overhead %.0f ticks (%.1f ns), subtracted below\n", overhead, overhead / TicksPerNs());
  LatencyBreakdown<FastBook>("optimized OrderBook", msgs, overhead);
  LatencyBreakdown<ReferenceBook>("std::map/list reference", msgs, overhead);

  std::printf("\npipeline: gateway thread (cpu %d) -> SPSC ring -> matching thread (cpu %d)\n", cpu_a, cpu_b);
  PipelineThroughput(msgs, cpu_a, cpu_b);
  Pipeline(msgs, cpu_a, cpu_b, 1e6);
  return trades_fast == trades_ref ? 0 : 1;
}
