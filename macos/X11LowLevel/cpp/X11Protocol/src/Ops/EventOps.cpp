//
//  EventOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <chrono>


#include "Ops/EventOps.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Core/XProtoContext.hpp"
#include "Utils/ByteReader.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/WireLE.hpp"
#include "Core/WindowTable.hpp"
#include "Core/X11Modifiers.hpp"
#include "Core/timestamp.hpp"
#include "Core/XI2EventMask.hpp"
#include "Core/X11ExtOpcodes.hpp"
#include "Core/InputState.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/XConstants.hpp"     // kPointerRootFocus (focusFlagFor)
#include "Core/timestamp.hpp"
#include "Utils/WireEvents.hpp"
#include "Utils/MachTime.hpp"

namespace x11 {

static inline int clampi(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}
static inline int16_t clamp_i16(int v) {
  return (int16_t)clampi(v, -32768, 32767);
}

struct AbsGeom { int absX=0, absY=0; };

  static inline int16_t clamp16_i32(int32_t v) {
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
  }

// FP16.16 from a signed integer: multiply rather than shift (left-shifting
// a negative int32 is undefined before C++20; the value was right by luck) —
// xorg double_to_fp1616 (dix/inpututils.c:1044-1046).  Phase E, L12.
static inline uint32_t fp1616(int32_t v) {
  return (uint32_t)((int64_t)v * 65536);
}

static inline void getRootWH(x11::XProtoContext& ctx, int& outW, int& outH) {
  outW = 0;
  outH = 0;

  x11::WindowView rv{};
  if (ctx.windows().snapshot(x11::kRootWindowXid, rv)) {   // R1: was the literal 1
    outW = (int)rv.w;   // WindowView::w is uint16_t
    outH = (int)rv.h;   // WindowView::h is uint16_t
    if (outW <= 0) outW = 1;
    if (outH <= 0) outH = 1;
    return;
  }

  // Fallback: keep clamps sane even if root isn't in the table yet.
  outW = 32767;
  outH = 32767;
}

  
// Compute eventX/eventY in target-local coords using host-local pointer coords.
// This avoids needing absolute on-screen window origin (rootless-friendly).
static bool computeEventXYFromHostLocal(x11::XProtoContext& ctx,
                                       uint32_t targetWid,
                                       int16_t* outEx,
                                       int16_t* outEy)
{
  if (!outEx || !outEy) return false;

  // Host whose win_x/win_y are defined (last motion host).  With no such
  // host (fresh server, or the last motion was a window-less tracker tick
  // that left last_xid = 0) the cached local coords describe nothing —
  // report failure so callers derive coords from the root position instead
  // of treating (0,0)-relative values as this window's (v1.20.0.33).
  const uint32_t host = ctx.input().last_xid;
  if (host == 0) return false;

  int32_t lx = ctx.input().win_x_u;
  int32_t ly = ctx.input().win_y_u;

  // Walk from target up to host, subtracting offsets
  uint32_t cur = targetWid;
  int safety = 0;
  while (cur && cur != host) {
    x11::WindowView cv{};
    if (!ctx.windows().snapshot(cur, cv)) return false;
    lx -= (cv.x + cv.border_width);
    ly -= (cv.y + cv.border_width);
    cur = cv.parent_xid;
    if (++safety > 64) return false;
  }
  if (cur != host && targetWid != host) return false;

  *outEx = clamp16_i32(lx);
  *outEy = clamp16_i32(ly);
  return true;
}

// Compute eventX/eventY using root (screen) coordinates.
// This is the fallback when the target is in a different host tree than the
// current motion host (e.g., popup menu on a separate NSWindow while macOS
// routes drag events to the original window).
// Walks from target to root, accumulating absolute position, then subtracts.
static bool computeEventXYFromRoot(x11::XProtoContext& ctx,
                                   uint32_t targetWid,
                                   int32_t root_x, int32_t root_y,
                                   int16_t* outEx, int16_t* outEy)
{
  if (!outEx || !outEy) return false;

  // Accumulate absolute position by walking target → root
  int32_t absX = 0, absY = 0;
  uint32_t cur = targetWid;
  int safety = 0;
  while (cur && cur != x11::kRootWindowXid) { // root XID
    x11::WindowView cv{};
    if (!ctx.windows().snapshot(cur, cv)) return false;
    absX += cv.x + cv.border_width;
    absY += cv.y + cv.border_width;
    cur = cv.parent_xid;
    if (++safety > 64) return false;
  }

  *outEx = clamp16_i32(root_x - absX);
  *outEy = clamp16_i32(root_y - absY);
  return true;
}  
  

// Final write of a core event.  To an explicit client (grab-window delivery
// goes to the grabbing client, xorg dix/events.c:4364 — also the only way to
// reach a root-window grab, which has no owner); otherwise, Phase C (M5), to
// every client that selected `bit` on the window — xorg DeliverEventsToWindow
// (dix/events.c:2342-2383) tries the owner and then each otherClients entry —
// each with its own sequence.  When nobody selected (the WM-emulation focus
// sites and the callers that decided deliverability by the union), the
// window's owner receives it as before.
// `ownerFallback`: motion/button/key callers decide deliverability by the
// window's union mask, so an empty selector list can only mean an
// inconsistency and the owner gets it; crossing and focus events (Phase D,
// M18) are strictly mask-gated, as DeliverEventsToWindow makes them.
static inline void emitCore(x11::XProtoContext& ctx, uint32_t wid, const uint8_t ev[32],
                            uint32_t bit, int toFd, bool ownerFallback = true) {
  if (toFd >= 0) { (void)ctx.transport().sendEventToFd(toFd, ev, 32); return; }
  const std::vector<int> fds = ctx.windows().selectorsOf(wid, bit);
  if (fds.empty()) {
    if (ownerFallback) (void)ctx.transport().sendEvent32(wid, ev);
    return;
  }
  for (int fd : fds) (void)ctx.transport().sendEventToFd(fd, ev, 32);
}

// Crossing/focus `focus` flag: the event window is the focus window, an
// inferior of it, or focus is PointerRoot (dix/events.c:4740-4744).
static inline bool focusFlagFor(x11::XProtoContext& ctx, uint32_t wid) {
  const uint32_t focusXid = ctx.input().focus_xid;
  if (focusXid == 0) return false;
  if (focusXid == x11::kPointerRootFocus) return true;   // PointerRoot
  uint32_t cur = wid;
  for (int depth = 0; cur != 0 && depth < 64; depth++) {
    if (cur == focusXid) return true;
    x11::WindowView v{};
    if (!ctx.windows().snapshot(cur, v)) break;
    cur = v.parent_xid;
  }
  return false;
}

// Core mask bits for the motion family: any of these on a window means the
// client wants MotionNotify (the caller already applied the button rule).
static constexpr uint32_t kCoreMotionBits =
    x11::mask::PointerMotion | x11::mask::ButtonMotion | (0x1Fu << 8);

// Final write of an XI2 event (Phase C, M4/M5).  Explicit client (grab) and
// `force` keep their Phase B meaning.  Otherwise: every client whose
// selection on `wid` carries `bit` for the event's device — xorg
// DeliverEventToInputClients over the window's InputClients list
// (dix/events.c:2203-2285) — each with its own sequence.  For the event
// types that propagate (button, motion, key), xorg's DeliverDeviceEvents walk
// ends at ROOT, an ordinary window whose selectors receive the event with
// event=root and child=the toplevel (FixUpEventFromWindow) — since R1 Phase 3
// (A5) the callers' walks include the root, so `wid` IS root in that case and
// its selection is looked up like any other (the Phase C post-hoc root
// fallback, with its re-addressing and the "not the window's own selection"
// exception, is gone).  Crossing and focus events are delivered to their
// window only (DeviceEnterLeaveEvent / DeviceFocusEvent →
// DeliverEventsToWindow, no walk).  Returns xorg's "deliveries > 0", which the
// callers use to suppress the core twin.
static bool deliverXI2(x11::XProtoContext& ctx, uint32_t wid, uint8_t* buf, size_t len,
                       uint32_t bit, uint16_t deviceid, bool force, int toFd) {
  if (toFd >= 0) { (void)ctx.transport().sendEventToFd(toFd, buf, len); return true; }
  if (force)     { (void)ctx.transport().sendEventVariable(wid, buf, len); return true; }

  const std::vector<int> fds = ctx.windows().xi2SelectorsOf(wid, bit, deviceid);
  if (fds.empty()) return false;
  for (int fd : fds) (void)ctx.transport().sendEventToFd(fd, buf, len);
  return true;
}

static void buildButtonEvent32(uint8_t ev[32],
                               uint8_t type,            // 4 press, 5 release
                               uint16_t seqField,        // "last request seq" is OK here
                               uint32_t timeMs,
                               uint32_t rootXid,
                               uint32_t eventWid,
                               uint32_t childWid,
                               int eventX, int eventY,   // relative to eventWid
                               uint16_t state,           // state BEFORE press / BEFORE release
                               uint8_t buttonDetail,
                               int rootW, int rootH,
                               AbsGeom abs)
{
  std::memset(ev, 0, 32);
  ev[0] = type;
  ev[1] = buttonDetail;
  wire::wr16_le(ev + 2, seqField);
  wire::wr32_le(ev + 4, timeMs);
  wire::wr32_le(ev + 8, rootXid);
  wire::wr32_le(ev + 12, eventWid);
  wire::wr32_le(ev + 16, childWid);

  // Root coords derived from abs + event, then clamped to root bounds
  int rx = abs.absX + eventX;
  int ry = abs.absY + eventY;

  // Clamp to advertised root size (keep inside screen)
  rx = clampi(rx, 0, rootW > 0 ? (rootW - 1) : 0);
  ry = clampi(ry, 0, rootH > 0 ? (rootH - 1) : 0);

  wire::wr16_le(ev + 20, (uint16_t)clamp_i16(rx));
  wire::wr16_le(ev + 22, (uint16_t)clamp_i16(ry));
  wire::wr16_le(ev + 24, (uint16_t)clamp_i16(eventX));
  wire::wr16_le(ev + 26, (uint16_t)clamp_i16(eventY));
  wire::wr16_le(ev + 28, state);

  ev[30] = 1; // same_screen = TRUE
}

static void buildMotionEvent32(uint8_t ev[32],
                               uint16_t seqField,
                               uint32_t timeMs,
                               uint32_t rootXid,
                               uint32_t eventWid,
                               uint32_t childWid,
                               int eventX, int eventY,
                               uint16_t state,
                               int rootW, int rootH,
                               AbsGeom abs)
{
  std::memset(ev, 0, 32);
  ev[0] = 6; // MotionNotify
  ev[1] = 0; // detail unused for MotionNotify in core
  wire::wr16_le(ev + 2, seqField);
  wire::wr32_le(ev + 4, timeMs);
  wire::wr32_le(ev + 8, rootXid);
  wire::wr32_le(ev + 12, eventWid);
  wire::wr32_le(ev + 16, childWid);

  int rx = abs.absX + eventX;
  int ry = abs.absY + eventY;
  rx = clampi(rx, 0, rootW > 0 ? (rootW - 1) : 0);
  ry = clampi(ry, 0, rootH > 0 ? (rootH - 1) : 0);

  wire::wr16_le(ev + 20, (uint16_t)clamp_i16(rx));
  wire::wr16_le(ev + 22, (uint16_t)clamp_i16(ry));
  wire::wr16_le(ev + 24, (uint16_t)clamp_i16(eventX));
  wire::wr16_le(ev + 26, (uint16_t)clamp_i16(eventY));
  wire::wr16_le(ev + 28, state);
  ev[30] = 1; // same_screen
}  
  
 
static void buildFocusEvent32(uint8_t ev[32],
                              uint8_t type,       // 9 FocusIn, 10 FocusOut
                              uint8_t detail,     // NotifyDetail (use 3 = NotifyNonlinear for bring-up)
                              uint16_t seqField,
                              uint32_t eventWid,
                              uint8_t mode,       // NotifyMode (0 = NotifyNormal)
                              uint8_t same_screen // 1
)
{
  std::memset(ev, 0, 32);
  ev[0] = type;
  ev[1] = detail;
  wire::wr16_le(ev + 2, seqField);
  wire::wr32_le(ev + 4, eventWid);
  ev[8] = mode;
  ev[9] = same_screen ? 1 : 0;
  // ev[10..31] remain 0
}  
  
  
void EventOps::handle(uint8_t majorOpcode, uint8_t minorOpcode, ByteReader& br) {
  // For now, no core “event opcodes” exist in core X11.
  // This is here for symmetry + future growth.
  ctx_.tracef("[EventOps] handle major=%u minor=%u (stub, skipping %zu)\n",
              (unsigned)majorOpcode, (unsigned)minorOpcode, br.remaining());
  br.skip(br.remaining());
}

//std::array<uint8_t, 32> EventOps::buildExpose(uint16_t seq,
//                                              uint32_t window,
//                                              uint16_t x, uint16_t y,
//                                              uint16_t w, uint16_t h,
//                                              uint16_t count) {
//  std::array<uint8_t, 32> ev{};
//  ev.fill(0);
//  
//  // Expose event type = 12
//  ev[0] = 12;
//  // ev[1] unused
//  wire::wr16_le(ev.data() + 2, seq);
//  wire::wr32_le(ev.data() + 4, window);
//  wire::wr16_le(ev.data() + 8, x);
//  wire::wr16_le(ev.data() + 10, y);
//  wire::wr16_le(ev.data() + 12, w);
//  wire::wr16_le(ev.data() + 14, h);
//  wire::wr16_le(ev.data() + 16, count);
//  return ev;
//}

std::array<uint8_t, 32> EventOps::buildConfigureNotify(const ConfigureNotifyParams& p)
{
  std::array<uint8_t, 32> ev{};
  ev.fill(0);
  
  // ConfigureNotify event type = 22
  ev[0] = 22;
  ev[1] = 0; // not synthetic
  wire::wr16_le(ev.data() + 2, p.seq);
  
  // event + window both set to window for a normal ConfigureNotify
  wire::wr32_le(ev.data() + 4, p.window); // event
  wire::wr32_le(ev.data() + 8, p.window); // window
  wire::wr32_le(ev.data() + 12, p.aboveSibling); // aboveSibling or None(0)
  
  // x/y are INT16 on the wire
  wire::wr16_le(ev.data() + 16, static_cast<uint16_t>(p.x));
  wire::wr16_le(ev.data() + 18, static_cast<uint16_t>(p.y));
  wire::wr16_le(ev.data() + 20, p.w);
  wire::wr16_le(ev.data() + 22, p.h);
  wire::wr16_le(ev.data() + 24, p.borderWidth);
  ev[26] = p.overrideRedirect ? 1 : 0;
  
  return ev;
}

void EventOps::queueExpose(uint32_t window,
                           uint16_t x, uint16_t y,
                           uint16_t w, uint16_t h,
                           uint16_t count)
{
  // Enqueue a request for the xproto thread to emit an Expose for this window.
  // The Transport layer owns the actual fd and sequence tracking.
  ctx_.transport().queueNotify(window, /*wantConfigure*/false, /*wantExpose*/true);
  
  ctx_.tracef("[EventOps] queueExpose wid=0x%08X rect=(%u,%u %ux%u) count=%u\n",
              (unsigned)window,
              (unsigned)x, (unsigned)y,
              (unsigned)w, (unsigned)h,
              (unsigned)count);
  
  // NOTE: for now we ignore the rect in the queued notify and emit a full-window Expose
  // during flush (matches current bring-up behavior). We will carry rect later.
  (void)x; (void)y; (void)w; (void)h; (void)count;
}

void EventOps::queueConfigureNotify(const ConfigureNotifyParams& p)
{
  // For now we only queue the fact that a ConfigureNotify should be emitted.
  // The transport flush pass will build the actual wire event using the latest
  // window geometry from XProtoContext.
  ctx_.transport().queueNotify(p.window, /*wantConfigure*/true, /*wantExpose*/false);
  
  ctx_.tracef("[EventOps] queueConfigureNotify wid=0x%08X xy=(%d,%d) wh=(%u,%u)\n",
              (unsigned)p.window,
              (int)p.x, (int)p.y,
              (unsigned)p.w, (unsigned)p.h);
  
  // NOTE: We currently do not carry the full ConfigureNotify payload through the pending queue.
  // Transport will emit the event using current window geometry at flush time.
  (void)p.borderWidth;
  (void)p.aboveSibling;
  (void)p.overrideRedirect;
  (void)p.seq;
}

void EventOps::queueConfigureNotify(uint32_t window,
                                    int16_t x, int16_t y,
                                    uint16_t w, uint16_t h,
                                    uint16_t borderWidth,
                                    uint32_t aboveSibling,
                                    bool overrideRedirect)
{
  ConfigureNotifyParams p;
  p.window = window;
  p.x = x;
  p.y = y;
  p.w = w;
  p.h = h;
  p.borderWidth = borderWidth;
  p.aboveSibling = aboveSibling;
  p.overrideRedirect = overrideRedirect;
  queueConfigureNotify(p);
}

void EventOps::flushPendingNotify(const PendingNotify& pn, uint16_t seq) {
  if (pn.wid == 0) return;
  
  const WindowView* w = ctx_.window(pn.wid);
  if (!w) return;
  
  // Only send what the client asked for (mask check mirrors C).
  if (pn.want_configure) {
    ConfigureNotifyParams p;
    p.seq = seq;
    p.window = pn.wid;
    p.x = w->x;
    p.y = w->y;
    p.w = static_cast<uint16_t>(w->w);
    p.h = static_cast<uint16_t>(w->h);
    p.borderWidth = w->border_width;
    p.aboveSibling = 0;
    p.overrideRedirect = w->override_redirect;
    const uint32_t parentXid = w->parent_xid;   // copy before any further ctx_.window() lookup
    auto ev = buildConfigureNotify(p);
    if ((w->event_mask & (1u << 17)) && w->owner_fd > 0) {
      ctx_.transport().sendEventToSelectors(pn.wid, x11::mask::StructureNotify, ev.data());
    }
    // R1 Phase 2 (A2): a ConfigureNotify also reaches the parent's
    // SubstructureNotify selectors with event=parent (xorg DeliverEvents,
    // dix/events.c:2969-2972).  This is the Cocoa-driven
    // move/resize path, so for a top-level the parent is the root window —
    // wmctrl/xdotool/Java's root observer track geometry through it.
    if (parentXid != 0) {
      WindowView pv{};
      if (ctx_.windows().snapshot(parentXid, pv) && (pv.event_mask & x11::mask::SubstructureNotify)) {
        auto pev = ev;
        wire::wr32_le(pev.data() + 4, parentXid);   // event = parent
        ctx_.transport().sendEventToSelectors(parentXid, x11::mask::SubstructureNotify, pev.data());
      }
    }
  }
  
  const bool selectedExposure = (w->event_mask & x11::mask::Exposure) != 0;

  // BRING-UP OVERRIDE: if PendingNotify asks for expose, send it once even if mask isn't set yet.
  // This fixes flaky first paint for xeyes/xterm.
  const bool forceExposeBringup = pn.want_expose != 0;

  if (selectedExposure || forceExposeBringup) {
    // Use the specific rect if available, otherwise full-window expose.
    uint16_t ex = 0, ey = 0;
    uint16_t ew = static_cast<uint16_t>(w->w);
    uint16_t eh = static_cast<uint16_t>(w->h);
    if (pn.expose_has_rect) {
      ex = pn.expose_x; ey = pn.expose_y;
      ew = pn.expose_w; eh = pn.expose_h;
    }
    auto ev = wireev::buildExpose(seq, pn.wid, ex, ey, ew, eh, pn.expose_count);
    ctx_.transport().sendEvent32(pn.wid, ev.data());
  }
  
}

void EventOps::sendMotionNotify(XProtoContext& ctx,
                                uint32_t wid,
                                int32_t root_x, int32_t root_y,
                                uint32_t buttons, uint32_t mods,
                                int toFd,
                                uint32_t child_xid)
{
  // event-local coords
  int16_t ex = 0, ey = 0;
  if (!computeEventXYFromHostLocal(ctx, wid, &ex, &ey)) {
    // Host-local failed (target in different host tree, e.g., popup menu
    // while macOS routes drag to original window). Use root-relative path.
    if (!computeEventXYFromRoot(ctx, wid, root_x, root_y, &ex, &ey)) {
      ex = clamp16_i32(ctx.input().win_x_u);
      ey = clamp16_i32(ctx.input().win_y_u);
    }
  }

  int rootW = 0, rootH = 0;
  getRootWH(ctx, rootW, rootH);

  // Ensure builder's root coords match what Swift gave us:
  // builder does root = abs + event => abs = root - event
  AbsGeom abs{};
  abs.absX = (int)(root_x - (int32_t)ex);
  abs.absY = (int)(root_y - (int32_t)ey);

  const uint16_t st = x11::input::toX11State(buttons, mods);

  uint8_t ev[32];
  buildMotionEvent32(ev,
                     ctx.transport().lastSeq(),
                     x11_now_ms_monotonic(),
                     x11::kRootWindowXid,   // root (R1: was the literal 1)
                     wid,    // event window
                     child_xid, // child of the event window on the sprite path
                     (int)ex, (int)ey,
                     st,
                     rootW, rootH,
                     abs);

  emitCore(ctx, wid, ev, kCoreMotionBits, toFd);
}


void EventOps::sendButtonEvent(XProtoContext& ctx,
                               uint32_t wid,
                               bool is_press,
                               uint8_t button,
                               int32_t root_x, int32_t root_y,
                               uint32_t buttons, uint32_t mods,
                               uint32_t child_xid,
                               int toFd)
{
  // event-local coords
  int16_t ex = 0, ey = 0;
  if (!computeEventXYFromHostLocal(ctx, wid, &ex, &ey)) {
    // Host-local failed (target in different host tree, e.g., popup menu
    // while macOS routes button to original window). Use root-relative path.
    if (!computeEventXYFromRoot(ctx, wid, root_x, root_y, &ex, &ey)) {
      ex = clamp16_i32(ctx.input().win_x_u);
      ey = clamp16_i32(ctx.input().win_y_u);
    }
  }

  int rootW = 0, rootH = 0;
  getRootWH(ctx, rootW, rootH);

  AbsGeom abs{};
  abs.absX = (int)(root_x - (int32_t)ex);
  abs.absY = (int)(root_y - (int32_t)ey);

  const uint16_t st = x11::input::toX11State(buttons, mods);

  uint8_t ev[32];
  buildButtonEvent32(ev,
                     is_press ? 4 : 5,
                     ctx.transport().lastSeq(),
                     x11_now_ms_monotonic(),
                     x11::kRootWindowXid,   // root (R1: was the literal 1)
                     wid,
                     child_xid,
                     (int)ex, (int)ey,
                     st,
                     button,
                     rootW, rootH,
                     abs);

  emitCore(ctx, wid, ev, is_press ? x11::mask::ButtonPress : x11::mask::ButtonRelease, toFd);
}


void EventOps::sendKeyEvent(XProtoContext& ctx,
                            uint32_t wid,
                            bool is_press,
                            uint8_t keycode,
                            uint32_t buttons, uint32_t mods,
                            int toFd)
{
  uint8_t ev[32] = {0};

  ev[0] = is_press ? 2 : 3; // KeyPress=2, KeyRelease=3
  ev[1] = keycode;          // detail = keycode (bring-up)
  wire::wr16_le(ev + 2, ctx.transport().lastSeq());
  wire::wr32_le(ev + 4, x11_now_ms_monotonic()); // time (ms)

  wire::wr32_le(ev + 8, x11::kRootWindowXid);    // root (R1: was the literal 1)
  wire::wr32_le(ev + 12, wid); // event
  wire::wr32_le(ev + 16, 0);   // child

  // rootX/rootY and eventX/eventY: use last known pointer root position if you want.
  // Clamp to 16-bit signed coordinate range used by core events
  auto clamp16 = [](int32_t v) -> int16_t {
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
  };
  // xorg pairs a key event with the master pointer's sprite position
  // (Xi/exevents.c:1860-1862): root coords, and event coords relative to
  // the event window — Phase E, L6.
  int16_t rx = clamp16(ctx.input().root_x_u);
  int16_t ry = clamp16(ctx.input().root_y_u);
  // Root position minus the window's root origin, exactly as xorg's
  // FixUpEventFromWindow does; the pointer is usually outside the focus
  // window's host, so the host-local cache is the wrong reference.
  int16_t ex = 0, ey = 0;
  if (!computeEventXYFromRoot(ctx, wid, ctx.input().root_x_u, ctx.input().root_y_u, &ex, &ey) &&
      !computeEventXYFromHostLocal(ctx, wid, &ex, &ey)) {
    ex = 0; ey = 0;
  }
  wire::wr16_le(ev + 20, (uint16_t)rx);
  wire::wr16_le(ev + 22, (uint16_t)ry);
  wire::wr16_le(ev + 24, (uint16_t)ex);
  wire::wr16_le(ev + 26, (uint16_t)ey);

  const uint16_t st = x11::input::toX11State(buttons, mods);
  wire::wr16_le(ev + 28, st); // state
  ev[30] = 1;
  ev[31] = 0;

  // Log AFTER computing state so we can see exactly what xterm/AWT will
  // observe in the wire payload.  modsRaw is the internal bit layout
  // (Shift=0, Ctrl=1, Alt=2, Cmd=3); state is the X11 wire layout
  // (Shift=0x1, Ctrl=0x4, Mod1=0x8, Mod4=0x40, button bits at 0x100+).
  // For Ctrl+V we expect state to have bit 2 (0x0004) set on the V event.
#ifndef NDEBUG
  TS_FPRINTF("[KEY_SEND] fd=%d wid=0x%08X focus=0x%08X type=%d kc=%u "
             "modsRaw=0x%02X state=0x%04X seq=%u\n",
             ctx.transport().clientFd(),
             (unsigned)wid,
             (unsigned)ctx.input().focus_xid,
             (int)ev[0],
             (unsigned)keycode,
             (unsigned)(mods & 0xFFu),
             (unsigned)st,
             (unsigned)ctx.transport().lastSeq());
#endif

  emitCore(ctx, wid, ev, is_press ? x11::mask::KeyPress : x11::mask::KeyRelease, toFd);
}

// xorg CoreFocusEvent / CoreEnterLeaveEvent (dix/events.c:4864-4877,
// 4754-4771): a FocusIn or EnterNotify to a window whose selection includes
// KeymapState is followed by a KeymapNotify carrying key->down[1..31], i.e.
// keycodes 8-255 — Phase G, L19.  KeymapNotify has no sequence number.
static void sendKeymapNotifyIfSelected(x11::XProtoContext& ctx, uint32_t wid, int toFd) {
  const x11::WindowView* wv = ctx.window(wid);
  if (!wv || !(wv->event_mask & x11::mask::KeymapState)) return;
  uint8_t ev[32] = {0};
  ev[0] = 11;   // KeymapNotify
  std::memcpy(ev + 1, ctx.input().getKeymap() + 1, 31);
  emitCore(ctx, wid, ev, x11::mask::KeymapState, toFd, /*ownerFallback=*/false);
}

void EventOps::sendCrossingEvent(XProtoContext& ctx,
                                uint32_t wid,
                                bool is_enter,
                                int32_t root_x, int32_t root_y,
                                uint32_t buttons, uint32_t mods,
                                uint8_t mode,
                                int toFd,
                                uint8_t detail,
                                uint32_t child)
{
  auto clamp16 = [](int32_t v) -> int16_t {
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
  };

  const int16_t rx = clamp16(root_x);
  const int16_t ry = clamp16(root_y);

  uint8_t ev[32] = {0};

  ev[0] = is_enter ? 7 : 8;   // EnterNotify=7, LeaveNotify=8
  ev[1] = detail;             // from the window relation (Utils/EnterLeave.hpp, M15)
  wire::wr16_le(ev + 2, ctx.transport().lastSeq());
  wire::wr32_le(ev + 4, x11_now_ms_monotonic()); // time

  wire::wr32_le(ev + 8, x11::kRootWindowXid);    // root (R1: was the literal 1)
  wire::wr32_le(ev + 12, wid); // event
  wire::wr32_le(ev + 16, child); // child: None on the endpoints, the path window on Virtual events

  wire::wr16_le(ev + 20, (uint16_t)rx); // rootX
  wire::wr16_le(ev + 22, (uint16_t)ry); // rootY

  // eventX/eventY must be window-local
  int16_t ex = 0, ey = 0;
  if (!computeEventXYFromHostLocal(ctx, wid, &ex, &ey)) {
    ex = clamp16(ctx.input().win_x_u);
    ey = clamp16(ctx.input().win_y_u);
  }
  wire::wr16_le(ev + 24, (uint16_t)ex);
  wire::wr16_le(ev + 26, (uint16_t)ey);

  const uint16_t st = x11::input::toX11State(buttons, mods);
  wire::wr16_le(ev + 28, st);

  ev[30] = mode; // mode: 0=Normal, 1=NotifyGrab, 2=NotifyUngrab

  // Byte 31 is a packed flags byte: bit0 = focus, bit1 = same_screen.
  // (The old `ev[31] = 1` claimed focus=True + same_screen=False on every
  // crossing — toolkits taking the "different screen" branch ignored the
  // coordinates.)  focus = event window is the focus window or an inferior.
  ev[31] = (uint8_t)(0x02 | (focusFlagFor(ctx, wid) ? 0x01 : 0x00)); // same_screen=True | focus

#ifdef X11_TRACE_VERBOSE
  TS_FPRINTF("[CROSS] wid=0x%08X %s detail=%u time=%u root=(%d,%d) event=(%d,%d) state=0x%04X\n",
          (unsigned)wid, is_enter ? "Enter" : "Leave", (unsigned)detail,
          (unsigned)wire::rd32_le(ev + 4),
          (int)rx, (int)ry, (int)ex, (int)ey, (unsigned)st);
#endif

  // M18: strictly mask-gated (xorg CoreEnterLeaveEvent, dix/events.c:4722)
  emitCore(ctx, wid, ev, is_enter ? x11::mask::EnterWindow : x11::mask::LeaveWindow, toFd,
           /*ownerFallback=*/false);
  if (is_enter) sendKeymapNotifyIfSelected(ctx, wid, toFd);   // L19
}
  
  
  
void EventOps::sendFocusEvent(XProtoContext& ctx, uint32_t wid, bool is_in,
                              uint8_t mode, uint8_t detail)
{
  if (!wid) return;

  const x11::WindowView* vw = ctx.window(wid);
  if (!vw) return;   // R1 Phase 3: no owner test — root (owner -1) selects FocusChange like any window

  // Cheap gate on the union; emitCore delivers to the selecting clients only
  // (M18 — xorg CoreFocusEvent → DeliverEventsToWindow, dix/events.c:4862).
  // The old sendFocusEventDirect bypassed the mask for the WM-emulation
  // sites; xorg delivers a WM's FocusIn only because the shell selected
  // FocusChange, and every toolkit we host does (xterm's shell, GTK/Chromium
  // toplevels, AWT focus proxies — checked 2026-09-07).
  if (!(vw->event_mask & x11::mask::FocusChange)) return;

  uint8_t ev[32];
  const uint8_t type = is_in ? 9 : 10;   // FocusIn / FocusOut
  const uint8_t same = 1;

  buildFocusEvent32(ev,
                    type,
                    detail,
                    ctx.transport().lastSeq(),
                    wid,
                    mode,
                    same);

  emitCore(ctx, wid, ev, x11::mask::FocusChange, -1, /*ownerFallback=*/false);
  if (is_in) sendKeymapNotifyIfSelected(ctx, wid, -1);   // L19
}


// ============================================================================
// XI2 (XInput2) GenericEvent senders
// ============================================================================

// Helpers for building XI2 wire events
namespace {

// Build button mask word from internal button bits (0-4 → X11 wire bits 1-5)
static uint32_t xi2ButtonMask(uint32_t buttons) {
  // xorg event_set_state (dix/inpututils.c:784-786) sets a bit for every
  // held button; the core pointer has 10 (buttons 6/7 = horizontal wheel,
  // which the core `state` field cannot carry) — Phase E, L2.
  uint32_t mask = 0;
  for (int i = 0; i < 10; i++) {
    if (buttons & (1u << i))
      mask |= (1u << (i + 1));  // XI2 button bits are 1-indexed
  }
  return mask;
}

// Fill mods section (4×uint32 = 16 bytes) at buf.  Caps Lock is a locked
// modifier in XKB state (dix/inpututils.c:798-803: base/latched/locked, with
// `effective` their union) — Phase E, L24.
static void fillXI2Mods(uint8_t* buf, uint32_t mods_state) {
  const uint32_t x11mods = x11::input::toX11State(0, mods_state) & 0xFFu;
  const uint32_t locked  = x11mods & 0x02u;          // LockMask
  x11::wire::wr32_le(buf + 0,  x11mods & ~locked);   // base
  x11::wire::wr32_le(buf + 4,  0);                   // latched
  x11::wire::wr32_le(buf + 8,  locked);              // locked
  x11::wire::wr32_le(buf + 12, x11mods);             // effective
}

// Fill group section (4×uint8 = 4 bytes) at buf
static void fillXI2Group(uint8_t* buf) {
  buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 0;
}

} // anonymous namespace

bool EventOps::sendXI2MotionEvent(XProtoContext& ctx, uint32_t wid,
                                  int32_t root_x, int32_t root_y,
                                  uint32_t buttons, uint32_t mods,
                                  bool force, int toFd,
                                  uint32_t child_xid) {
  const WindowView* wv = ctx.window(wid);
  if (!force && !wv) return false;
  // Phase C: the window's union gates cheaply; deliverXI2 picks the clients.
  if (!force && !(wv->xi2_mask & xi2::kMotionMask)) return false;

  // Compute event-local coords using proper hierarchy walk (same as core events)
  int16_t ex = 0, ey = 0;
  if (!computeEventXYFromHostLocal(ctx, wid, &ex, &ey)) {
    if (!computeEventXYFromRoot(ctx, wid, root_x, root_y, &ex, &ey)) {
      ex = clamp16_i32(ctx.input().win_x_u);
      ey = clamp16_i32(ctx.input().win_y_u);
    }
  }

  uint8_t buf[xi2::kDeviceEventSize] = {};
  buf[0] = 35;                                          // GenericEvent
  buf[1] = (uint8_t)ext::kXInput2;                      // extension
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());   // sequence
  wire::wr32_le(buf + 4,  xi2::kDeviceEventLength);     // length
  wire::wr16_le(buf + 8,  xi2::kMotion);                // evtype
  wire::wr16_le(buf + 10, xi2::kVirtualCorePointer);    // deviceid
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());      // time
  wire::wr32_le(buf + 16, 0);                           // detail (0 for motion)
  wire::wr32_le(buf + 20, x11::kRootWindowXid);                           // root window
  wire::wr32_le(buf + 24, wid);                         // event window
  wire::wr32_le(buf + 28, child_xid);                   // child (FixUpEventFromWindow)
  wire::wr32_le(buf + 32, fp1616(root_x));    // root_x FP16.16
  wire::wr32_le(buf + 36, fp1616(root_y));    // root_y FP16.16
  wire::wr32_le(buf + 40, fp1616(ex)); // event_x FP16.16
  wire::wr32_le(buf + 44, fp1616(ey)); // event_y FP16.16
  wire::wr16_le(buf + 48, xi2::kXIButtonsLen);          // buttons_len = 8 (xorg)
  wire::wr16_le(buf + 50, xi2::kXIValuatorsLen);        // valuators_len = 2 (xorg)
  wire::wr16_le(buf + 52, xi2::kRealPointer);          // sourceid = real slave pointer (not XTEST)
  // buf[54-55] = pad
  wire::wr32_le(buf + 56, 0);                           // flags
  fillXI2Mods(buf + 60, mods);                           // mods (16 bytes)
  fillXI2Group(buf + 76);                                // group (4 bytes)
  // Trailing (buf is zero-init): 32B button mask + 8B valuator mask + 2×FP3232.
  wire::wr32_le(buf + 80, xi2ButtonMask(buttons));       // button mask, word 0
  buf[112] = 0x03;                                       // valuator mask bits 0,1 (x,y)
  wire::wr32_le(buf + 120, (uint32_t)root_x);            // valuator 0 = x (FP3232 integral)
  wire::wr32_le(buf + 128, (uint32_t)root_y);            // valuator 1 = y (FP3232 integral)

  return deliverXI2(ctx, wid, buf, sizeof(buf), xi2::kMotionMask,
                    xi2::kVirtualCorePointer, force, toFd);
}

bool EventOps::sendXI2ButtonEvent(XProtoContext& ctx, uint32_t wid,
                                  bool is_press, uint8_t button,
                                  int32_t root_x, int32_t root_y,
                                  uint32_t buttons, uint32_t mods,
                                  uint32_t child_xid,
                                  bool force, int toFd) {
  uint32_t mask_bit = is_press ? xi2::kButtonPressMask : xi2::kButtonReleaseMask;
  const WindowView* wv = ctx.window(wid);
  if (!force && !wv) return false;
  if (!force && !(wv->xi2_mask & mask_bit)) return false;

  // Compute event-local coords using proper hierarchy walk (same as core events)
  int16_t ex = 0, ey = 0;
  if (!computeEventXYFromHostLocal(ctx, wid, &ex, &ey)) {
    if (!computeEventXYFromRoot(ctx, wid, root_x, root_y, &ex, &ey)) {
      ex = clamp16_i32(ctx.input().win_x_u);
      ey = clamp16_i32(ctx.input().win_y_u);
    }
  }

  uint8_t buf[xi2::kDeviceEventSize] = {};
  buf[0] = 35;
  buf[1] = (uint8_t)ext::kXInput2;
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());
  wire::wr32_le(buf + 4,  xi2::kDeviceEventLength);
  wire::wr16_le(buf + 8,  is_press ? xi2::kButtonPress : xi2::kButtonRelease);
  wire::wr16_le(buf + 10, xi2::kVirtualCorePointer);
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());
  wire::wr32_le(buf + 16, (uint32_t)button);             // detail = button number
  wire::wr32_le(buf + 20, x11::kRootWindowXid);                            // root
  wire::wr32_le(buf + 24, wid);                          // event
  wire::wr32_le(buf + 28, child_xid);                    // child
  wire::wr32_le(buf + 32, fp1616(root_x));
  wire::wr32_le(buf + 36, fp1616(root_y));
  wire::wr32_le(buf + 40, fp1616(ex));
  wire::wr32_le(buf + 44, fp1616(ey));
  wire::wr16_le(buf + 48, xi2::kXIButtonsLen);           // buttons_len = 8 (xorg)
  wire::wr16_le(buf + 50, xi2::kXIValuatorsLen);         // valuators_len = 2 (xorg)
  wire::wr16_le(buf + 52, xi2::kRealPointer);           // sourceid = real slave pointer (not XTEST)
  wire::wr32_le(buf + 56, 0);                            // flags
  fillXI2Mods(buf + 60, mods);
  fillXI2Group(buf + 76);
  // Trailing: 32B button mask + 8B valuator mask + 2×FP3232 (x,y).
  wire::wr32_le(buf + 80, xi2ButtonMask(buttons));       // button mask, word 0
  buf[112] = 0x03;                                       // valuator mask bits 0,1 (x,y)
  wire::wr32_le(buf + 120, (uint32_t)root_x);            // valuator 0 = x (FP3232 integral)
  wire::wr32_le(buf + 128, (uint32_t)root_y);            // valuator 1 = y (FP3232 integral)

  return deliverXI2(ctx, wid, buf, sizeof(buf), mask_bit,
                    xi2::kVirtualCorePointer, force, toFd);
}

bool EventOps::sendXI2KeyEvent(XProtoContext& ctx, uint32_t wid,
                               bool is_press, uint8_t keycode,
                               uint32_t buttons, uint32_t mods,
                               bool force, int toFd, bool repeat) {
  uint32_t mask_bit = is_press ? xi2::kKeyPressMask : xi2::kKeyReleaseMask;
  const WindowView* wv = ctx.window(wid);
  if (!force && !wv) return false;
  if (!force && !(wv->xi2_mask & mask_bit)) return false;

  // xorg pairs every key event with the master pointer's sprite position
  // and button state (Xi/exevents.c:1860-1862, dix/inpututils.c:784-786) —
  // Phase E, L6.
  const int32_t root_x = ctx.input().root_x_u, root_y = ctx.input().root_y_u;
  int16_t ex = 0, ey = 0;
  if (!computeEventXYFromRoot(ctx, wid, root_x, root_y, &ex, &ey) &&
      !computeEventXYFromHostLocal(ctx, wid, &ex, &ey)) {
    ex = 0; ey = 0;
  }

  uint8_t buf[xi2::kKeyEventSize] = {};
  buf[0] = 35;
  buf[1] = (uint8_t)ext::kXInput2;
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());
  wire::wr32_le(buf + 4,  xi2::kKeyEventLength);
  wire::wr16_le(buf + 8,  is_press ? xi2::kKeyPress : xi2::kKeyRelease);
  wire::wr16_le(buf + 10, xi2::kVirtualCoreKeyboard);
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());
  wire::wr32_le(buf + 16, (uint32_t)keycode);
  wire::wr32_le(buf + 20, x11::kRootWindowXid);   // root
  wire::wr32_le(buf + 24, wid);
  wire::wr32_le(buf + 28, 0);   // child
  wire::wr32_le(buf + 32, fp1616(root_x));
  wire::wr32_le(buf + 36, fp1616(root_y));
  wire::wr32_le(buf + 40, fp1616(ex));
  wire::wr32_le(buf + 44, fp1616(ey));
  wire::wr16_le(buf + 48, xi2::kXIButtonsLen);   // buttons_len = 8 (xorg)
  wire::wr16_le(buf + 50, xi2::kXIValuatorsLen); // valuators_len = 2 (xorg)
  wire::wr16_le(buf + 52, xi2::kRealKeyboard);          // sourceid = real slave keyboard (not XTEST)
  // flags: XIKeyRepeat (1 << 16) on an autorepeat press (dix/eventconvert.c:714-715) — M11
  wire::wr32_le(buf + 56, (is_press && repeat) ? (1u << 16) : 0u);
  fillXI2Mods(buf + 60, mods);
  fillXI2Group(buf + 76);
  // Trailing: 32B button mask (paired pointer's held buttons) + 8B valuator
  // mask (all zero: keys carry no valuators, so no FP3232 values follow).
  wire::wr32_le(buf + 80, xi2ButtonMask(buttons));

  return deliverXI2(ctx, wid, buf, sizeof(buf), mask_bit,
                    xi2::kVirtualCoreKeyboard, force, toFd);
}

bool EventOps::sendXI2CrossingEvent(XProtoContext& ctx, uint32_t wid,
                                    bool is_enter,
                                    int32_t root_x, int32_t root_y,
                                    uint32_t buttons, uint32_t mods,
                                    uint8_t mode,
                                    bool force, int toFd,
                                    uint8_t detail, uint32_t child) {
  uint32_t mask_bit = is_enter ? xi2::kEnterMask : xi2::kLeaveMask;
  const WindowView* wv = ctx.window(wid);
  if (!force && !wv) return false;
  if (!force && !(wv->xi2_mask & mask_bit)) return false;

  // Compute event-local coords using proper hierarchy walk (same as core events)
  int16_t ex = 0, ey = 0;
  if (!computeEventXYFromHostLocal(ctx, wid, &ex, &ey)) {
    if (!computeEventXYFromRoot(ctx, wid, root_x, root_y, &ex, &ey)) {
      ex = clamp16_i32(ctx.input().win_x_u);
      ey = clamp16_i32(ctx.input().win_y_u);
    }
  }

  uint8_t buf[xi2::kEnterEventSize] = {};
  buf[0] = 35;
  buf[1] = (uint8_t)ext::kXInput2;
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());
  wire::wr32_le(buf + 4,  xi2::kEnterEventLength);
  wire::wr16_le(buf + 8,  is_enter ? xi2::kEnter : xi2::kLeave);
  wire::wr16_le(buf + 10, xi2::kVirtualCorePointer);
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());
  wire::wr16_le(buf + 16, xi2::kRealPointer);           // sourceid = real slave pointer (not XTEST)
  buf[18] = mode;   // mode: 0=Normal, 1=NotifyGrab, 2=NotifyUngrab
  buf[19] = detail; // from the window relation (Utils/EnterLeave.hpp, M15)
  wire::wr32_le(buf + 20, x11::kRootWindowXid);                            // root
  wire::wr32_le(buf + 24, wid);                          // event
  wire::wr32_le(buf + 28, child);                        // child (path window on Virtual events)
  wire::wr32_le(buf + 32, fp1616(root_x));
  wire::wr32_le(buf + 36, fp1616(root_y));
  wire::wr32_le(buf + 40, fp1616(ex));
  wire::wr32_le(buf + 44, fp1616(ey));
  buf[48] = 1;   // same_screen = True
  buf[49] = focusFlagFor(ctx, wid) ? 1 : 0;   // focus, as the core sender (enterleave.c DeviceEnterLeaveEvent)
  wire::wr16_le(buf + 50, xi2::kXIButtonsLen);          // buttons_len = 8 (xorg)
  fillXI2Mods(buf + 52, mods);                            // mods (16 bytes)
  fillXI2Group(buf + 68);                                 // group (4 bytes)
  // Trailing: 32B button mask (buttons_len=8), word 0 carries the buttons.
  // xXIEnterEvent has no valuators.
  wire::wr32_le(buf + 72, xi2ButtonMask(buttons));        // button mask, word 0

  return deliverXI2(ctx, wid, buf, sizeof(buf), mask_bit,
                    xi2::kVirtualCorePointer, force, toFd);
}

void EventOps::sendXI2FocusEvent(XProtoContext& ctx, uint32_t wid, bool is_in,
                                 uint8_t mode, uint8_t detail) {
  uint32_t mask_bit = is_in ? xi2::kFocusInMask : xi2::kFocusOutMask;
  const WindowView* wv = ctx.window(wid);
  if (!wv) return;
  if (!(wv->xi2_mask & mask_bit)) return;

  uint8_t buf[xi2::kEnterEventSize] = {};
  buf[0] = 35;
  buf[1] = (uint8_t)ext::kXInput2;
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());
  wire::wr32_le(buf + 4,  xi2::kEnterEventLength);
  wire::wr16_le(buf + 8,  is_in ? xi2::kFocusIn : xi2::kFocusOut);
  wire::wr16_le(buf + 10, xi2::kVirtualCoreKeyboard);
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());
  wire::wr16_le(buf + 16, xi2::kRealKeyboard);           // sourceid = real slave keyboard (not XTEST)
  buf[18] = mode;    // 0=Normal 1=Grab 2=Ungrab 3=WhileGrabbed (xorg DoFocusEvents)
  buf[19] = detail;  // NotifyNonlinear for toplevel transitions; M9 computes the rest
  wire::wr32_le(buf + 20, x11::kRootWindowXid);   // root
  wire::wr32_le(buf + 24, wid);
  wire::wr32_le(buf + 28, 0);   // child
  // coordinates = 0 for focus events
  buf[48] = 1;   // same_screen
  buf[49] = 1;   // focus = True (for focus events)
  wire::wr16_le(buf + 50, xi2::kXIButtonsLen);   // buttons_len = 8 (xorg)
  fillXI2Mods(buf + 52, 0);
  fillXI2Group(buf + 68);
  // Trailing: 32B button mask (all zero for focus events).

  (void)deliverXI2(ctx, wid, buf, sizeof(buf), mask_bit,
                   xi2::kVirtualCoreKeyboard, /*force=*/false, /*toFd=*/-1);
}

void EventOps::sendXI2RawMotionEvent(XProtoContext& ctx) {
  // Cheap gate on root's union before building anything (R1 Phase 3: root's
  // XI2 selections are its WindowTable entries like any window's).
  const WindowView* rv = ctx.window(x11::kRootWindowXid);
  if (!rv || !(rv->xi2_mask & xi2::kRawMotionMask)) return;

  // Phase C: xorg stamps raw events with the slave that produced them
  // (mi/mieq.c:331-340) and matches root selections through xi2mask_isset,
  // so XIAllDevices and the slave id match; xeyes selects with
  // XIAllMasterDevices and works on xorg because the master processes the
  // event as well.  Our raw event carries the master id, so accept selections
  // for the master and for its real slave.
  std::vector<int> fds = ctx.windows().xi2SelectorsOf(x11::kRootWindowXid, xi2::kRawMotionMask, xi2::kVirtualCorePointer);
  for (int fd : ctx.windows().xi2SelectorsOf(x11::kRootWindowXid, xi2::kRawMotionMask, xi2::kRealPointer)) {
    bool dup = false;
    for (int f : fds) if (f == fd) { dup = true; break; }
    if (!dup) fds.push_back(fd);
  }
  if (fds.empty()) return;

  // xorg DeliverRawEvent (dix/events.c:2464-2488): raw events go to every
  // client that selected them on root, regardless of the window under the
  // pointer.  Until v1.20.0.13 this was routed to the OWNER of the last
  // active host window (GlobalPointerTracker.activeXid), so xeyes froze
  // whenever the pointer was over Vitis and Chromium received RawMotion it
  // never selected.  The sequence is restamped per target by sendEventToFd.

  // The core pointer's axes are relative (Rel X / Rel Y, dix/devices.c:
  // 662-663), so the valuators carry the delta since the previous raw event
  // (GetPointerEvents: raw = device delta, values = the accelerated delta;
  // no acceleration here, so both are the same) — Phase E, L4.  A move
  // that does not change the root position is no move at all in xorg.
  auto& in = ctx.input();
  const int32_t dx = in.raw_have ? in.root_x_u - in.raw_last_x : 0;
  const int32_t dy = in.raw_have ? in.root_y_u - in.raw_last_y : 0;
  const bool first = !in.raw_have;
  in.raw_last_x = in.root_x_u; in.raw_last_y = in.root_y_u; in.raw_have = true;
  // The first position only seeds the delta (no previous position, no
  // motion to report); xorg sends nothing without motion (v1.20.0.33).
  if (first || (dx == 0 && dy == 0)) return;

  // xXIRawEvent (xorg eventToRawEvent, dix/eventconvert.c:768-808):
  //   32-byte header + valuator mask (valuators_len = 2 words, MAX_VALUATORS
  //   = 36) + values[] (accelerated) + raw_values[], one FP3232 per set
  //   valuator = 72 bytes, length 10.  libXi's XInputWireToCookie needs
  //   length > 0 to allocate the cookie.
  uint8_t buf[xi2::kRawEventSize] = {};
  buf[0] = 35;                                         // GenericEvent
  buf[1] = (uint8_t)ext::kXInput2;                     // extension
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());  // sequence
  wire::wr32_le(buf + 4,  xi2::kRawEventLength);       // length = 10
  wire::wr16_le(buf + 8,  xi2::kRawMotion);            // evtype = 17
  wire::wr16_le(buf + 10, xi2::kVirtualCorePointer);   // deviceid
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());     // time
  wire::wr32_le(buf + 16, 0);                          // detail = 0
  wire::wr16_le(buf + 20, xi2::kRealPointer);          // sourceid = real slave pointer (not XTEST)
  wire::wr16_le(buf + 22, xi2::kXIValuatorsLen);       // valuators_len = 2 (xorg)
  wire::wr32_le(buf + 24, 0);                          // flags = 0
  // buf[28-31] = pad (already 0)

  buf[32] = 0x03;                                      // valuator mask: bits 0+1 (X, Y); word 1 zero
  wire::wr32_le(buf + 40, (uint32_t)dx);               // values[0]     = X (FP3232 integral, frac 0)
  wire::wr32_le(buf + 48, (uint32_t)dy);               // values[1]     = Y
  wire::wr32_le(buf + 56, (uint32_t)dx);               // raw_values[0] = X
  wire::wr32_le(buf + 64, (uint32_t)dy);               // raw_values[1] = Y

  for (int fd : fds) (void)ctx.transport().sendEventToFd(fd, buf, sizeof(buf));
}

// A raw event with no valuators: 32-byte header + the 2-word valuator mask
// (all zero) = 40 bytes, length 2 (xorg eventToRawEvent with an empty mask,
// dix/eventconvert.c:768-808).  Root selectors for the master and for the
// slave that produced it, as for RawMotion.
static void sendXI2RawSimple(x11::XProtoContext& ctx, uint16_t evtype, uint32_t maskBit,
                             uint16_t deviceid, uint16_t sourceid, uint32_t detail) {
  const x11::WindowView* rv = ctx.window(x11::kRootWindowXid);
  if (!rv || !(rv->xi2_mask & maskBit)) return;
  std::vector<int> fds = ctx.windows().xi2SelectorsOf(x11::kRootWindowXid, maskBit, deviceid);
  for (int fd : ctx.windows().xi2SelectorsOf(x11::kRootWindowXid, maskBit, sourceid)) {
    bool dup = false;
    for (int f : fds) if (f == fd) { dup = true; break; }
    if (!dup) fds.push_back(fd);
  }
  if (fds.empty()) return;

  uint8_t buf[40] = {};
  buf[0] = 35;                                         // GenericEvent
  buf[1] = (uint8_t)x11::ext::kXInput2;
  x11::wire::wr16_le(buf + 2,  ctx.transport().lastSeq());
  x11::wire::wr32_le(buf + 4,  2);                     // length: 2 mask words
  x11::wire::wr16_le(buf + 8,  evtype);
  x11::wire::wr16_le(buf + 10, deviceid);
  x11::wire::wr32_le(buf + 12, x11_now_ms_monotonic());
  x11::wire::wr32_le(buf + 16, detail);                // button / keycode
  x11::wire::wr16_le(buf + 20, sourceid);
  x11::wire::wr16_le(buf + 22, x11::xi2::kXIValuatorsLen);
  x11::wire::wr32_le(buf + 24, 0);                     // flags
  // buf[28..31] pad, buf[32..39] valuator mask (no axes)
  for (int fd : fds) (void)ctx.transport().sendEventToFd(fd, buf, sizeof(buf));
}

void EventOps::sendXI2RawButtonEvent(XProtoContext& ctx, bool is_press, uint8_t button) {
  sendXI2RawSimple(ctx,
                   is_press ? xi2::kRawButtonPress : xi2::kRawButtonRelease,
                   is_press ? xi2::kRawButtonPressMask : xi2::kRawButtonReleaseMask,
                   xi2::kVirtualCorePointer, xi2::kRealPointer, button);
}

void EventOps::sendXI2RawKeyEvent(XProtoContext& ctx, bool is_press, uint8_t keycode) {
  sendXI2RawSimple(ctx,
                   is_press ? xi2::kRawKeyPress : xi2::kRawKeyRelease,
                   is_press ? xi2::kRawKeyPressMask : xi2::kRawKeyReleaseMask,
                   xi2::kVirtualCoreKeyboard, xi2::kRealKeyboard, keycode);
}

} // namespace x11
