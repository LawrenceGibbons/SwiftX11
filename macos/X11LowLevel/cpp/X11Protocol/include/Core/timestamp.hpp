//
//  timestamp.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/17/26.
//

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Monotonic time in nanoseconds.
// On macOS uses mach_continuous_time() (monotonic, includes sleep).
uint64_t x11_now_ns_monotonic();

// Monotonic time in milliseconds, forced strictly increasing (never returns 0).
uint32_t x11_now_ms_monotonic();

#ifdef __cplusplus
} // extern "C"
#endif

