//
//  MachTime.hpp
//  X11LowLevel
//
//  Mach absolute time → seconds, matching CACurrentMediaTime() used by
//  the Swift log window.  Use machTimeSeconds() for stderr timestamps
//  so they can be correlated with the Swift UI log.
//

#pragma once

#include <mach/mach_time.h>

namespace x11 {
namespace util {

inline double machTimeSeconds() {
  static mach_timebase_info_data_t tb = {};
  if (tb.denom == 0) mach_timebase_info(&tb);
  return static_cast<double>(mach_absolute_time()) * tb.numer / tb.denom / 1e9;
}

} // namespace util
} // namespace x11

// Convenience macro: fprintf to stderr with a [timestamp] prefix.
// Use for key diagnostic messages that need to correlate with the Swift log.
#define TS_FPRINTF(fmt, ...) \
  fprintf(stderr, "[%.6f] " fmt, x11::util::machTimeSeconds(), ##__VA_ARGS__)

// Debug-only variant — compiles to nothing in Release builds (NDEBUG defined
// by Xcode's Release configuration).  Use for high-frequency diagnostic
// messages that aren't useful in production stderr output, like per-op
// selection / cursor / lifecycle traces.  Keep TS_FPRINTF for messages
// that must always appear (version banner, fatal errors, X11_ERROR).
#ifndef NDEBUG
#define TS_DBG(fmt, ...) TS_FPRINTF(fmt, ##__VA_ARGS__)
#else
#define TS_DBG(fmt, ...) ((void)0)
#endif
