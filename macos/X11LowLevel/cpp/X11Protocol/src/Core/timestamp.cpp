//
//  timestamp.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/17/26.
//

#include "timestamp.hpp"

#include <atomic>

#if defined(__APPLE__)
  #include <mach/mach_time.h>
#else
  #include <time.h>
#endif

uint64_t x11_now_ns_monotonic() {
#if defined(__APPLE__)
  static mach_timebase_info_data_t tb{};
  static std::atomic<uint8_t> inited{0};

  if (inited.load(std::memory_order_acquire) == 0) {
    mach_timebase_info(&tb);
    inited.store(1, std::memory_order_release);
  }

  const uint64_t t = mach_continuous_time(); // monotonic, includes sleep
  __uint128_t ns = (__uint128_t)t * (__uint128_t)tb.numer;
  ns /= (__uint128_t)tb.denom;
  return (uint64_t)ns;
#else
  // Best-effort Linux/BSD:
  // - CLOCK_BOOTTIME includes suspend time on Linux (closer to mach_continuous_time semantics).
  // - Fallback to CLOCK_MONOTONIC otherwise.
  struct timespec ts{};
  #if defined(CLOCK_BOOTTIME)
    const clockid_t clk = CLOCK_BOOTTIME;
  #else
    const clockid_t clk = CLOCK_MONOTONIC;
  #endif

  if (clock_gettime(clk, &ts) != 0) return 0;

  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

uint32_t x11_now_ms_monotonic() {
  static std::atomic<uint64_t> last_ms{0};

  uint64_t ms = x11_now_ns_monotonic() / 1000000ull;
  if (ms == 0) ms = 1; // never 0

  uint64_t prev = last_ms.load(std::memory_order_relaxed);
  while (true) {
    uint64_t want = (ms <= prev) ? (prev + 1) : ms;
    if (last_ms.compare_exchange_weak(prev, want,
                                     std::memory_order_release,
                                     std::memory_order_relaxed)) {
      ms = want;
      break;
    }
    // prev updated; loop
  }

  return (uint32_t)(ms & 0xFFFFFFFFu);
}
