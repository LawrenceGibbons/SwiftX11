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
inline std::atomic<uint32_t> g_motions{0};      // delivered to a target
inline std::atomic<uint32_t> g_raw_motions{0};  // arrived at postMotion at all
inline std::atomic<double>   g_start_s{0.0};
inline std::atomic<bool>     g_active{false};

inline void begin(uint32_t host, uint32_t drag_xid, uint32_t button) {
#if X11_TRACE_DRAG_ENABLED
  g_host.store(host, std::memory_order_relaxed);
  g_drag_xid.store(drag_xid, std::memory_order_relaxed);
  g_button.store(button, std::memory_order_relaxed);
  g_motions.store(0, std::memory_order_relaxed);
  g_raw_motions.store(0, std::memory_order_relaxed);
  g_start_s.store(x11::util::machTimeSeconds(), std::memory_order_relaxed);
  g_active.store(true, std::memory_order_release);
  TS_FPRINTF("[DRAG] BEGIN  host=0x%08X drag_xid=0x%08X button=%u\n",
             (unsigned)host, (unsigned)drag_xid, (unsigned)button);
#else
  (void)host; (void)drag_xid; (void)button;
#endif
}

// Called at the very top of postMotion — counts every motion event that
// reached our server from Cocoa, regardless of whether it ultimately gets
// delivered to an X11 target.  Pair with motion() (called after target
// resolution) to detect motion events that get dropped due to deliver=0
// or target=0 after host correction.
//
// `live_drag_xid` is the CURRENT value of InputState::drag_xid at the
// moment this motion arrives — distinct from g_drag_xid (captured once
// at begin()).  Comparing the two lets us see whether drag_xid got
// silently cleared somewhere mid-drag (e.g., by a stray removeClient,
// a button() side effect, or some other unexpected path).
inline void motionRaw(uint32_t host_xid,
                      int32_t root_x, int32_t root_y,
                      uint8_t deliver, uint32_t buttons,
                      uint32_t live_drag_xid) {
#if X11_TRACE_DRAG_ENABLED
  if (!g_active.load(std::memory_order_acquire)) return;
  const uint32_t n = g_raw_motions.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 3 || (n % 10) == 0) {
    TS_FPRINTF("[DRAG] raw    #%u host=0x%08X root=(%d,%d) "
               "deliver=%u buttons=0x%02X drag_xid=0x%08X\n",
               (unsigned)n, (unsigned)host_xid,
               (int)root_x, (int)root_y,
               (unsigned)deliver, (unsigned)(buttons & 0xFFu),
               (unsigned)live_drag_xid);
  }
#else
  (void)host_xid; (void)root_x; (void)root_y;
  (void)deliver; (void)buttons; (void)live_drag_xid;
#endif
}

// Called at each early-return point inside postMotion.  `reason` is a
// short literal identifying the return site.  We sample the same way
// as raw/motion (first 3, then every 10th) so the log shows pattern
// without flooding for long drags.
inline void dropped(const char* reason) {
#if X11_TRACE_DRAG_ENABLED
  if (!g_active.load(std::memory_order_acquire)) return;
  // Lazy local counter, encoded into the high bits of the address-derived
  // hash so each reason has its own per-drag count.  Simplest version:
  // log every call up to 3 per reason, suppress thereafter.
  // (Thread-locality avoids cross-drag contamination because drags are
  // single-threaded on the xproto thread.)
  static thread_local const char* names[8] = {nullptr};
  static thread_local uint32_t counts[8] = {0};
  static thread_local uint32_t last_raw[8] = {0};
  const uint32_t n_raw = g_raw_motions.load(std::memory_order_relaxed);
  int slot = -1;
  for (int i = 0; i < 8; i++) {
    if (names[i] == reason) { slot = i; break; }
    if (names[i] == nullptr) { names[i] = reason; counts[i] = 0; last_raw[i] = 0; slot = i; break; }
  }
  if (slot < 0) return;
  // Reset slot's count when the raw counter went backwards (new drag).
  if (n_raw < last_raw[slot]) counts[slot] = 0;
  last_raw[slot] = n_raw;
  const uint32_t c = ++counts[slot];
  if (c <= 3 || (c % 10) == 0) {
    TS_FPRINTF("[DRAG] DROP   reason=%s count=%u at raw=%u\n",
               reason, (unsigned)c, (unsigned)n_raw);
  }
#else
  (void)reason;
#endif
}

// Called inside postMotion's host-correction loop when a higher-stacking
// top-level window is picked as the effective host.  Logs which window
// won so we can spot the case where a retained XDND proxy (or some other
// unexpected window) silently steals motion routing away from drag_xid.
inline void hostCorr(uint32_t original_host, uint32_t corrected_host,
                     uint32_t corrected_w, uint32_t corrected_h,
                     int32_t corrected_x, int32_t corrected_y) {
#if X11_TRACE_DRAG_ENABLED
  if (!g_active.load(std::memory_order_acquire)) return;
  TS_FPRINTF("[DRAG] HOST_CORR orig=0x%08X → 0x%08X (size=%ux%u origin=(%d,%d))\n",
             (unsigned)original_host, (unsigned)corrected_host,
             (unsigned)corrected_w, (unsigned)corrected_h,
             (int)corrected_x, (int)corrected_y);
#else
  (void)original_host; (void)corrected_host;
  (void)corrected_w; (void)corrected_h;
  (void)corrected_x; (void)corrected_y;
#endif
}

inline void motion(uint32_t target,
                   int32_t root_x, int32_t root_y,
                   uint32_t buttons, uint32_t mods) {
#if X11_TRACE_DRAG_ENABLED
  if (!g_active.load(std::memory_order_acquire)) return;
  const uint32_t n = g_motions.fetch_add(1, std::memory_order_relaxed) + 1;
  // Log first 3 motions verbatim then sample every 10th to keep the log
  // legible during a several-hundred-event drag.
  //
  // `buttons` / `mods` are the INTERNAL state bits (button 1 = bit 0;
  // mod bits Shift=0, Ctrl=1, Alt=2, Cmd=3).  These are what the call
  // site holds before x11::input::toX11State() maps them to the X11
  // wire layout (button 1 = bit 8 in the MotionNotify `state` field).
  // For the hw_ila drag bug we mostly want to know whether buttons==1
  // (i.e. button-1 still held) — AWT only treats motion as drag-
  // eligible when Button1Mask is in `state`.
  if (n <= 3 || (n % 10) == 0) {
    TS_FPRINTF("[DRAG] motion #%u target=0x%08X root=(%d,%d) "
               "buttons=0x%02X mods=0x%02X\n",
               (unsigned)n, (unsigned)target,
               (int)root_x, (int)root_y,
               (unsigned)(buttons & 0xFFu), (unsigned)(mods & 0xFFu));
  }
#else
  (void)target; (void)root_x; (void)root_y; (void)buttons; (void)mods;
#endif
}

inline void end(uint32_t button) {
#if X11_TRACE_DRAG_ENABLED
  if (!g_active.load(std::memory_order_acquire)) return;
  const uint32_t n_del = g_motions.load(std::memory_order_relaxed);
  const uint32_t n_raw = g_raw_motions.load(std::memory_order_relaxed);
  const double   t0    = g_start_s.load(std::memory_order_relaxed);
  const double   dt    = x11::util::machTimeSeconds() - t0;
  const uint32_t ms    = (dt > 0.0) ? (uint32_t)(dt * 1000.0 + 0.5) : 0;
  TS_FPRINTF("[DRAG] END    host=0x%08X drag_xid=0x%08X button=%u "
             "delivered=%u raw=%u dropped=%u duration=%ums\n",
             (unsigned)g_host.load(std::memory_order_relaxed),
             (unsigned)g_drag_xid.load(std::memory_order_relaxed),
             (unsigned)button, (unsigned)n_del, (unsigned)n_raw,
             (unsigned)(n_raw > n_del ? n_raw - n_del : 0u),
             (unsigned)ms);
  g_active.store(false, std::memory_order_release);
#else
  (void)button;
#endif
}

} // namespace drag_trace
} // namespace x11
