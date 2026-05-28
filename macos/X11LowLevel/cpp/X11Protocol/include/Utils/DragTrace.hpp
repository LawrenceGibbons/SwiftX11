//
//  DragTrace.hpp
//  X11LowLevel
//
//  Diagnostic helper for the hw_ila_x drag-and-drop bug (TODO.md).
//
//  The [DRAG] category, when enabled, instruments every button press /
//  release pair on the xproto thread with:
//
//    [DRAG] BEGIN  host=0x... drag_xid=0x... button=N
//    [DRAG] motion #K target=0x...      (first 3, then every 10th)
//    [DRAG] END    host=0x... drag_xid=0x... button=N motions=N duration=Xms
//
//  All accessors are thread-safe (std::atomic) because postMotion may run
//  on either the xproto thread (HostCommand drain) or be called from
//  XInput2 RawMotion paths.  Begin/end are called from the button
//  handler on the xproto thread only.
//
//  When X11_TRACE_DRAG_ENABLED is 0, every function compiles to nothing.
//

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

#include "Utils/MachTime.hpp"
#include "Utils/TraceDefs.hpp"

namespace x11 {
namespace drag_trace {

inline std::atomic<uint32_t> g_host{0};
inline std::atomic<uint32_t> g_drag_xid{0};
inline std::atomic<uint32_t> g_button{0};
inline std::atomic<uint32_t> g_motions{0};
inline std::atomic<double>   g_start_s{0.0};
inline std::atomic<bool>     g_active{false};

inline void begin(uint32_t host, uint32_t drag_xid, uint32_t button) {
#if X11_TRACE_DRAG_ENABLED
  g_host.store(host, std::memory_order_relaxed);
  g_drag_xid.store(drag_xid, std::memory_order_relaxed);
  g_button.store(button, std::memory_order_relaxed);
  g_motions.store(0, std::memory_order_relaxed);
  g_start_s.store(x11::util::machTimeSeconds(), std::memory_order_relaxed);
  g_active.store(true, std::memory_order_release);
  TS_FPRINTF("[DRAG] BEGIN  host=0x%08X drag_xid=0x%08X button=%u\n",
             (unsigned)host, (unsigned)drag_xid, (unsigned)button);
#else
  (void)host; (void)drag_xid; (void)button;
#endif
}

inline void motion(uint32_t target) {
#if X11_TRACE_DRAG_ENABLED
  if (!g_active.load(std::memory_order_acquire)) return;
  const uint32_t n = g_motions.fetch_add(1, std::memory_order_relaxed) + 1;
  // Log first 3 motions verbatim then sample every 10th to keep the log
  // legible during a several-hundred-event drag.
  if (n <= 3 || (n % 10) == 0) {
    TS_FPRINTF("[DRAG] motion #%u target=0x%08X\n",
               (unsigned)n, (unsigned)target);
  }
#else
  (void)target;
#endif
}

inline void end(uint32_t button) {
#if X11_TRACE_DRAG_ENABLED
  if (!g_active.load(std::memory_order_acquire)) return;
  const uint32_t n  = g_motions.load(std::memory_order_relaxed);
  const double   t0 = g_start_s.load(std::memory_order_relaxed);
  const double   dt = x11::util::machTimeSeconds() - t0;
  const uint32_t ms = (dt > 0.0) ? (uint32_t)(dt * 1000.0 + 0.5) : 0;
  TS_FPRINTF("[DRAG] END    host=0x%08X drag_xid=0x%08X button=%u "
             "motions=%u duration=%ums\n",
             (unsigned)g_host.load(std::memory_order_relaxed),
             (unsigned)g_drag_xid.load(std::memory_order_relaxed),
             (unsigned)button, (unsigned)n, (unsigned)ms);
  g_active.store(false, std::memory_order_release);
#else
  (void)button;
#endif
}

} // namespace drag_trace
} // namespace x11
