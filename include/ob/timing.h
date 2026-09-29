// Cycle-accurate timing for latency measurement, and thread pinning.
//
// On x86 the time-stamp counter is read with RDTSC fenced by LFENCE, so the
// measured region cannot leak across the timestamps through out-of-order
// execution. Cycles are converted to nanoseconds using a frequency calibrated
// against steady_clock. Other architectures fall back to steady_clock.
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64)
#define OB_HAVE_TSC 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace ob {

inline uint64_t ReadTicks() noexcept {
#ifdef OB_HAVE_TSC
  _mm_lfence();
  const uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#else
  return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Ticks per nanosecond, measured once.
inline double TicksPerNs() {
  static const double value = [] {
#ifdef OB_HAVE_TSC
    const auto c0 = std::chrono::steady_clock::now();
    const uint64_t t0 = ReadTicks();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t t1 = ReadTicks();
    const auto c1 = std::chrono::steady_clock::now();
    return static_cast<double>(t1 - t0) /
           static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
#else
    return 1.0 * std::chrono::steady_clock::period::den / 1e9 / std::chrono::steady_clock::period::num;
#endif
  }();
  return value;
}

// Pins the calling thread to one CPU. Returns false if unsupported or refused.
inline bool PinThisThread(int cpu) {
#ifdef _WIN32
  return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu) != 0;
#elif defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

}  // namespace ob
