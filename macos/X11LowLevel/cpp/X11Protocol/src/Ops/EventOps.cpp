//
//  EventOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstddef>
#include <cstdint>
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
#include "Core/timestamp.hpp"
#include "Utils/WireEvents.hpp"

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

static inline void getRootWH(x11::XProtoContext& ctx, int& outW, int& outH) {
  outW = 0;
  outH = 0;

  x11::WindowView rv{};
  if (ctx.windows().snapshot(1 /*root xid*/, rv)) {
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

  // Host whose win_x/win_y are defined (last motion host)
  uint32_t host = ctx.input().last_xid;
  if (host == 0) host = ctx.windows().topLevelAncestorOf(targetWid);
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
  while (cur && cur != 1) { // 1 = root XID (kRootXid)
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
    if ((w->event_mask & (1u << 17)) && w->owner_fd > 0) {
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
      auto ev = buildConfigureNotify(p);
      ctx_.transport().sendEvent32(pn.wid, ev.data());
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
                                uint32_t buttons, uint32_t mods)
{
  // event-local coords
  int16_t ex = 0, ey = 0;
  int coordPath = 0; // 1=hostLocal, 2=fromRoot, 3=fallback
  if (!computeEventXYFromHostLocal(ctx, wid, &ex, &ey)) {
    // Host-local failed (target in different host tree, e.g., popup menu
    // while macOS routes drag to original window). Use root-relative path.
    if (!computeEventXYFromRoot(ctx, wid, root_x, root_y, &ex, &ey)) {
      ex = clamp16_i32(ctx.input().win_x_u);
      ey = clamp16_i32(ctx.input().win_y_u);
      coordPath = 3;
    } else {
      coordPath = 2;
    }
  } else {
    coordPath = 1;
  }

  // Diagnostic: log motion events to popup/OR windows to diagnose menu highlighting
#ifndef NDEBUG
  {
    uint32_t tgt_host = ctx.windows().topLevelAncestorOf(wid);
    x11::WindowView hv{};
    bool isOR = (tgt_host && ctx.windows().snapshot(tgt_host, hv) && hv.override_redirect);
    if (isOR) {
      static int or_motion_count = 0;
      if (++or_motion_count <= 10 || (or_motion_count % 50) == 0) {
        fprintf(stderr, "[OR_MOTION] #%d wid=0x%08X host=0x%08X root=(%d,%d) ev=(%d,%d) path=%d lastHost=0x%08X\n",
                or_motion_count, (unsigned)wid, (unsigned)tgt_host,
                (int)root_x, (int)root_y, (int)ex, (int)ey,
                coordPath, (unsigned)ctx.input().last_xid);
      }
    }
  }
#endif

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
                     1,      // root
                     wid,    // event window
                     0,      // child
                     (int)ex, (int)ey,
                     st,
                     rootW, rootH,
                     abs);

  ctx.transport().sendEvent32(wid, ev);
}


void EventOps::sendButtonEvent(XProtoContext& ctx,
                               uint32_t wid,
                               bool is_press,
                               uint8_t button,
                               int32_t root_x, int32_t root_y,
                               uint32_t buttons, uint32_t mods,
                               uint32_t child_xid)
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
                     1,
                     wid,
                     child_xid,
                     (int)ex, (int)ey,
                     st,
                     button,
                     rootW, rootH,
                     abs);

  ctx.transport().sendEvent32(wid, ev);
}
  

void EventOps::sendKeyEvent(XProtoContext& ctx,
                            uint32_t wid,
                            bool is_press,
                            uint8_t keycode,
                            uint32_t buttons, uint32_t mods)
{
  uint8_t ev[32] = {0};

  ev[0] = is_press ? 2 : 3; // KeyPress=2, KeyRelease=3
  ev[1] = keycode;          // detail = keycode (bring-up)
  wire::wr16_le(ev + 2, ctx.transport().lastSeq());
  wire::wr32_le(ev + 4, x11_now_ms_monotonic()); // time (ms)

  wire::wr32_le(ev + 8, 1);    // root
  wire::wr32_le(ev + 12, wid); // event
  wire::wr32_le(ev + 16, 0);   // child

  // rootX/rootY and eventX/eventY: use last known pointer root position if you want.
  // Clamp to 16-bit signed coordinate range used by core events
  auto clamp16 = [](int32_t v) -> int16_t {
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
  };
  int16_t rx = clamp16(ctx.input().root_x_u);
  int16_t ry = clamp16(ctx.input().root_y_u);
  wire::wr16_le(ev + 20, (uint16_t)rx);
  wire::wr16_le(ev + 22, (uint16_t)ry);
  wire::wr16_le(ev + 24, (uint16_t)rx);
  wire::wr16_le(ev + 26, (uint16_t)ry);
  
  const uint16_t st = x11::input::toX11State(buttons, mods);
  wire::wr16_le(ev + 28, st); // state
  ev[30] = 1;
  ev[31] = 0;

  ctx.transport().sendEvent32(wid, ev);
}

void EventOps::sendCrossingEvent(XProtoContext& ctx,
                                uint32_t wid,
                                bool is_enter,
                                int32_t root_x, int32_t root_y,
                                uint32_t buttons, uint32_t mods)
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
  ev[1] = 0;                  // detail: NotifyAncestor (0) is fine for bring-up
  wire::wr16_le(ev + 2, ctx.transport().lastSeq());
  wire::wr32_le(ev + 4, x11_now_ms_monotonic()); // time

  wire::wr32_le(ev + 8, 1);    // root
  wire::wr32_le(ev + 12, wid); // event
  wire::wr32_le(ev + 16, 0);   // child (we don’t track subwindow crossings yet)

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

  ev[30] = 0; // mode: NotifyNormal (0)
  ev[31] = 1; // sameScreen (TRUE) — focus byte is folded into xCrossingEvent in some layouts,
              // but for our 32-byte wire event union this is acceptable for bring-up.

#ifdef X11_TRACE_VERBOSE
  fprintf(stderr, "[CROSS] wid=0x%08X %s time=%u root=(%d,%d) event=(%d,%d) state=0x%04X\n",
          (unsigned)wid, is_enter ? "Enter" : "Leave",
          (unsigned)wire::rd32_le(ev + 4),
          (int)rx, (int)ry, (int)ex, (int)ey, (unsigned)st);
#endif

  ctx.transport().sendEvent32(wid, ev);
}  
  
  
  
void EventOps::sendFocusEvent(XProtoContext& ctx, uint32_t wid, bool is_in)
{
  if (!wid) return;

  const x11::WindowView* vw = ctx.window(wid);
  if (!vw || vw->owner_fd <= 0) return;

  // Only deliver if client selected FocusChangeMask
  const bool wantFocus = (vw->event_mask & x11::mask::FocusChange) != 0;
  if (!wantFocus) return;

  uint8_t ev[32];
  const uint8_t type   = is_in ? 9 : 10;   // FocusIn / FocusOut
  const uint8_t detail = 3;                // NotifyNonlinear (good bring-up default)
  const uint8_t mode   = 0;                // NotifyNormal
  const uint8_t same   = 1;

  buildFocusEvent32(ev,
                    type,
                    detail,
                    ctx.transport().lastSeq(),
                    wid,
                    mode,
                    same);

  ctx.transport().sendEvent32(wid, ev);
}


void EventOps::sendFocusEventDirect(XProtoContext& ctx, uint32_t wid, bool is_in)
{
  if (!wid) return;

  const x11::WindowView* vw = ctx.window(wid);
  if (!vw || vw->owner_fd <= 0) return;

  // NOTE: No FocusChangeMask check — this emulates SetInputFocus behaviour
  // where the server always delivers FocusIn/FocusOut to the focus target.
  // Used by our rootless WM (Cocoa focus / click-to-focus) to ensure the
  // top-level shell widget receives FocusIn so it can propagate to children.

  uint8_t ev[32];
  const uint8_t type   = is_in ? 9 : 10;   // FocusIn / FocusOut
  const uint8_t detail = 3;                // NotifyNonlinear
  const uint8_t mode   = 0;                // NotifyNormal
  const uint8_t same   = 1;

  buildFocusEvent32(ev,
                    type,
                    detail,
                    ctx.transport().lastSeq(),
                    wid,
                    mode,
                    same);


  ctx.transport().sendEvent32(wid, ev);
}


// ============================================================================
// XI2 (XInput2) GenericEvent senders
// ============================================================================

// Helpers for building XI2 wire events
namespace {

// Build button mask word from internal button bits (0-4 → X11 wire bits 1-5)
static uint32_t xi2ButtonMask(uint32_t buttons) {
  uint32_t mask = 0;
  for (int i = 0; i < 5; i++) {
    if (buttons & (1u << i))
      mask |= (1u << (i + 1));  // XI2 button bits are 1-indexed
  }
  return mask;
}

// Fill mods section (4×uint32 = 16 bytes) at buf
static void fillXI2Mods(uint8_t* buf, uint32_t mods_state) {
  uint32_t x11mods = x11::input::toX11State(0, mods_state) & 0xFFu;
  x11::wire::wr32_le(buf + 0,  x11mods);  // base
  x11::wire::wr32_le(buf + 4,  0);        // latched
  x11::wire::wr32_le(buf + 8,  0);        // locked
  x11::wire::wr32_le(buf + 12, x11mods);  // effective
}

// Fill group section (4×uint8 = 4 bytes) at buf
static void fillXI2Group(uint8_t* buf) {
  buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 0;
}

} // anonymous namespace

void EventOps::sendXI2MotionEvent(XProtoContext& ctx, uint32_t wid,
                                  int32_t root_x, int32_t root_y,
                                  uint32_t buttons, uint32_t mods) {
  const WindowView* wv = ctx.window(wid);
  if (!wv) return;
  uint32_t eff_mask = wv->xi2_mask | ctx.input().xi2_root_mask;
  if (!(eff_mask & xi2::kMotionMask)) return;

  // Compute event-relative coords
  int32_t ev_x = root_x - wv->x;
  int32_t ev_y = root_y - wv->y;

  uint8_t buf[xi2::kDeviceEventSize] = {};
  buf[0] = 35;                                          // GenericEvent
  buf[1] = (uint8_t)ext::kXInput2;                      // extension
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());   // sequence
  wire::wr32_le(buf + 4,  xi2::kDeviceEventLength);     // length
  wire::wr16_le(buf + 8,  xi2::kMotion);                // evtype
  wire::wr16_le(buf + 10, xi2::kVirtualCorePointer);    // deviceid
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());         // time
  wire::wr32_le(buf + 16, 0);                           // detail (0 for motion)
  wire::wr32_le(buf + 20, 1);                           // root window
  wire::wr32_le(buf + 24, wid);                         // event window
  wire::wr32_le(buf + 28, 0);                           // child
  wire::wr32_le(buf + 32, (uint32_t)(root_x << 16));    // root_x FP16.16
  wire::wr32_le(buf + 36, (uint32_t)(root_y << 16));    // root_y FP16.16
  wire::wr32_le(buf + 40, (uint32_t)(ev_x << 16));      // event_x FP16.16
  wire::wr32_le(buf + 44, (uint32_t)(ev_y << 16));      // event_y FP16.16
  wire::wr16_le(buf + 48, 1);                           // buttons_len
  wire::wr16_le(buf + 50, 0);                           // valuators_len
  wire::wr16_le(buf + 52, xi2::kVirtualCorePointer);    // sourceid
  // buf[54-55] = pad
  wire::wr32_le(buf + 56, 0);                           // flags
  fillXI2Mods(buf + 60, mods);                              // mods (16 bytes)
  fillXI2Group(buf + 76);                                   // group (4 bytes)
  wire::wr32_le(buf + 80, xi2ButtonMask(buttons));       // button_mask

  ctx.transport().sendEventVariable(wid, buf, sizeof(buf));
}

void EventOps::sendXI2ButtonEvent(XProtoContext& ctx, uint32_t wid,
                                  bool is_press, uint8_t button,
                                  int32_t root_x, int32_t root_y,
                                  uint32_t buttons, uint32_t mods,
                                  uint32_t child_xid) {
  uint32_t mask_bit = is_press ? xi2::kButtonPressMask : xi2::kButtonReleaseMask;
  const WindowView* wv = ctx.window(wid);
  if (!wv) return;
  uint32_t eff_mask = wv->xi2_mask | ctx.input().xi2_root_mask;
  if (!(eff_mask & mask_bit)) return;

  int32_t ev_x = root_x - wv->x;
  int32_t ev_y = root_y - wv->y;

  uint8_t buf[xi2::kDeviceEventSize] = {};
  buf[0] = 35;
  buf[1] = (uint8_t)ext::kXInput2;
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());
  wire::wr32_le(buf + 4,  xi2::kDeviceEventLength);
  wire::wr16_le(buf + 8,  is_press ? xi2::kButtonPress : xi2::kButtonRelease);
  wire::wr16_le(buf + 10, xi2::kVirtualCorePointer);
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());
  wire::wr32_le(buf + 16, (uint32_t)button);             // detail = button number
  wire::wr32_le(buf + 20, 1);                            // root
  wire::wr32_le(buf + 24, wid);                          // event
  wire::wr32_le(buf + 28, child_xid);                    // child
  wire::wr32_le(buf + 32, (uint32_t)(root_x << 16));
  wire::wr32_le(buf + 36, (uint32_t)(root_y << 16));
  wire::wr32_le(buf + 40, (uint32_t)(ev_x << 16));
  wire::wr32_le(buf + 44, (uint32_t)(ev_y << 16));
  wire::wr16_le(buf + 48, 1);                            // buttons_len
  wire::wr16_le(buf + 50, 0);                            // valuators_len
  wire::wr16_le(buf + 52, xi2::kVirtualCorePointer);
  wire::wr32_le(buf + 56, 0);                            // flags
  fillXI2Mods(buf + 60, mods);
  fillXI2Group(buf + 76);
  wire::wr32_le(buf + 80, xi2ButtonMask(buttons));

  ctx.transport().sendEventVariable(wid, buf, sizeof(buf));
}

void EventOps::sendXI2KeyEvent(XProtoContext& ctx, uint32_t wid,
                               bool is_press, uint8_t keycode,
                               uint32_t buttons, uint32_t mods) {
  uint32_t mask_bit = is_press ? xi2::kKeyPressMask : xi2::kKeyReleaseMask;
  const WindowView* wv = ctx.window(wid);
  if (!wv) return;
  uint32_t eff_mask = wv->xi2_mask | ctx.input().xi2_root_mask;
  if (!(eff_mask & mask_bit)) return;

  uint8_t buf[xi2::kDeviceEventSize] = {};
  buf[0] = 35;
  buf[1] = (uint8_t)ext::kXInput2;
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());
  wire::wr32_le(buf + 4,  xi2::kDeviceEventLength);
  wire::wr16_le(buf + 8,  is_press ? xi2::kKeyPress : xi2::kKeyRelease);
  wire::wr16_le(buf + 10, xi2::kVirtualCoreKeyboard);
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());
  wire::wr32_le(buf + 16, (uint32_t)keycode);
  wire::wr32_le(buf + 20, 1);   // root
  wire::wr32_le(buf + 24, wid);
  wire::wr32_le(buf + 28, 0);   // child
  // Coordinates: 0 for keyboard events (no pointer position included)
  wire::wr16_le(buf + 48, 1);   // buttons_len
  wire::wr16_le(buf + 50, 0);   // valuators_len
  wire::wr16_le(buf + 52, xi2::kVirtualCoreKeyboard);
  wire::wr32_le(buf + 56, 0);   // flags
  fillXI2Mods(buf + 60, mods);
  fillXI2Group(buf + 76);
  wire::wr32_le(buf + 80, xi2ButtonMask(buttons));

  ctx.transport().sendEventVariable(wid, buf, sizeof(buf));
}

void EventOps::sendXI2CrossingEvent(XProtoContext& ctx, uint32_t wid,
                                    bool is_enter,
                                    int32_t root_x, int32_t root_y,
                                    uint32_t buttons, uint32_t mods) {
  uint32_t mask_bit = is_enter ? xi2::kEnterMask : xi2::kLeaveMask;
  const WindowView* wv = ctx.window(wid);
  if (!wv) return;
  uint32_t eff_mask = wv->xi2_mask | ctx.input().xi2_root_mask;
  if (!(eff_mask & mask_bit)) return;

  int32_t ev_x = root_x - wv->x;
  int32_t ev_y = root_y - wv->y;

  uint8_t buf[xi2::kEnterEventSize] = {};
  buf[0] = 35;
  buf[1] = (uint8_t)ext::kXInput2;
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());
  wire::wr32_le(buf + 4,  xi2::kEnterEventLength);
  wire::wr16_le(buf + 8,  is_enter ? xi2::kEnter : xi2::kLeave);
  wire::wr16_le(buf + 10, xi2::kVirtualCorePointer);
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());
  wire::wr16_le(buf + 16, xi2::kVirtualCorePointer);     // sourceid
  buf[18] = 0;   // mode = Normal
  buf[19] = 0;   // detail = Ancestor
  wire::wr32_le(buf + 20, 1);                            // root
  wire::wr32_le(buf + 24, wid);                          // event
  wire::wr32_le(buf + 28, 0);                            // child
  wire::wr32_le(buf + 32, (uint32_t)(root_x << 16));
  wire::wr32_le(buf + 36, (uint32_t)(root_y << 16));
  wire::wr32_le(buf + 40, (uint32_t)(ev_x << 16));
  wire::wr32_le(buf + 44, (uint32_t)(ev_y << 16));
  buf[48] = 1;   // same_screen = True
  buf[49] = 0;   // focus = False
  wire::wr16_le(buf + 50, 1);                            // buttons_len
  fillXI2Mods(buf + 52, mods);                               // mods (16 bytes)
  fillXI2Group(buf + 68);                                    // group (4 bytes)
  wire::wr32_le(buf + 72, xi2ButtonMask(buttons));        // button_mask

  ctx.transport().sendEventVariable(wid, buf, sizeof(buf));
}

void EventOps::sendXI2FocusEvent(XProtoContext& ctx, uint32_t wid, bool is_in) {
  uint32_t mask_bit = is_in ? xi2::kFocusInMask : xi2::kFocusOutMask;
  const WindowView* wv = ctx.window(wid);
  if (!wv) return;
  uint32_t eff_mask = wv->xi2_mask | ctx.input().xi2_root_mask;
  if (!(eff_mask & mask_bit)) return;

  uint8_t buf[xi2::kEnterEventSize] = {};
  buf[0] = 35;
  buf[1] = (uint8_t)ext::kXInput2;
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());
  wire::wr32_le(buf + 4,  xi2::kEnterEventLength);
  wire::wr16_le(buf + 8,  is_in ? xi2::kFocusIn : xi2::kFocusOut);
  wire::wr16_le(buf + 10, xi2::kVirtualCoreKeyboard);
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());
  wire::wr16_le(buf + 16, xi2::kVirtualCoreKeyboard);    // sourceid
  buf[18] = 0;   // mode = Normal
  buf[19] = 0;   // detail = Ancestor
  wire::wr32_le(buf + 20, 1);   // root
  wire::wr32_le(buf + 24, wid);
  wire::wr32_le(buf + 28, 0);   // child
  // coordinates = 0 for focus events
  buf[48] = 1;   // same_screen
  buf[49] = 1;   // focus = True (for focus events)
  wire::wr16_le(buf + 50, 1);   // buttons_len
  fillXI2Mods(buf + 52, 0);
  fillXI2Group(buf + 68);
  wire::wr32_le(buf + 72, 0);   // button_mask = 0

  ctx.transport().sendEventVariable(wid, buf, sizeof(buf));
}

void EventOps::sendXI2RawMotionEvent(XProtoContext& ctx, uint32_t wid) {
  // Only send if root has RawMotionMask
  if (!(ctx.input().xi2_root_mask & xi2::kRawMotionMask)) return;
  // Need a valid window for sendEventVariable's owner check
  const WindowView* wv = ctx.window(wid);
  if (!wv) return;

  // xXIRawEvent wire format with 2 valuators (X, Y):
  //   Header (32 bytes) + valuator_mask[1] (4) + raw[2]×8 + cooked[2]×8 = 68
  // libXi's XInputWireToCookie requires length>0 to allocate cookie data.
  uint8_t buf[xi2::kRawEventSize] = {};
  buf[0] = 35;                                         // GenericEvent
  buf[1] = (uint8_t)ext::kXInput2;                     // extension
  wire::wr16_le(buf + 2,  ctx.transport().lastSeq());  // sequence
  wire::wr32_le(buf + 4,  xi2::kRawEventLength);       // length = 9
  wire::wr16_le(buf + 8,  xi2::kRawMotion);            // evtype = 17
  wire::wr16_le(buf + 10, xi2::kVirtualCorePointer);   // deviceid
  wire::wr32_le(buf + 12, x11_now_ms_monotonic());     // time
  wire::wr32_le(buf + 16, 0);                          // detail = 0
  wire::wr16_le(buf + 20, xi2::kVirtualCorePointer);   // sourceid
  wire::wr16_le(buf + 22, 1);                          // valuators_len = 1 (one mask word)
  wire::wr32_le(buf + 24, 0);                          // flags = 0
  // buf[28-31] = pad (already 0)

  // Valuator data (36 bytes after the 32-byte header):
  wire::wr32_le(buf + 32, 0x03);                       // valuator_mask: bits 0+1 set (X, Y)
  // raw_values[0] = X delta (FP32.32) — set to 0 (we don't track deltas)
  // raw_values[1] = Y delta (FP32.32)
  // values[0] = X delta cooked (FP32.32)
  // values[1] = Y delta cooked (FP32.32)
  // All zero — xeyes only uses RawMotion as a trigger to call XQueryPointer

  ctx.transport().sendEventVariable(wid, buf, sizeof(buf));
}

} // namespace x11
