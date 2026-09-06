//
//  GrabRoute.hpp
//  X11LowLevel
//
//  Grab-time delivery decisions, mirroring xorg dix/events.c
//  DeliverGrabbedEvent / DeliverOneGrabbedEvent / CoreEnterLeaveEvent
//  (Phase B2, v1.20.0.18 — docs/XI2_XORG_COMPARISON.md M7 / M17).
//

#pragma once
#include <cstdint>

#include "Core/XProtoContext.hpp"
#include "Core/GrabTable.hpp"
#include "Core/WindowView.hpp"
#include "Core/XEventMask.hpp"
#include "Core/XI2EventMask.hpp"

namespace x11::grabroute {

struct Decision {
  uint32_t target  = 0;     // event window
  bool     viaGrab = false; // stage 2: grab window, grab level, grab mask, grab client
};

// xorg DeliverGrabbedEvent (dix/events.c:4399-4434).  Stage 1 runs only with
// owner_events: normal delivery from the sprite window — but TryClientEvents
// returns -1 for any client other than the grab's (:2039-2044) and
// DeliverDeviceEvents stops the walk there (:2892-2896), so the first window
// that selected the event must belong to the grabbing client.  Otherwise
// stage 2: DeliverOneGrabbedEvent to the grab window.  `normalTarget` is the
// window the ungrabbed walk would deliver to (0 = nobody selected it).
inline Decision route(XProtoContext& ctx, const PointerGrab& g, uint32_t normalTarget) {
  Decision d;
  if (g.ownerEvents && normalTarget) {
    const WindowView* vw = ctx.window(normalTarget);
    if (vw && vw->owner_fd == g.owner_fd) {
      d.target = normalTarget;
      return d;
    }
  }
  d.target  = g.grabWindow;
  d.viaGrab = true;
  return d;
}

// xorg FixUpEventFromWindow(…, child=None, calcChild=TRUE) (dix/events.c):
// the `child` field is the child of the EVENT window that is an ancestor of
// (or is) the sprite window — None when the sprite window is the event
// window itself or does not lie beneath it.  For a root-window grab this is
// the toplevel under the pointer, which AWT's XDND reads as the drop-target
// candidate (`xmotion.subwindow`); v1.20.0.18 sent None and the hw_ila drag
// never found a target.
inline uint32_t childOnSpritePath(XProtoContext& ctx, uint32_t eventWin, uint32_t spriteWin) {
  if (!spriteWin || !eventWin || spriteWin == eventWin) return 0;
  uint32_t w = spriteWin;
  for (int i = 0; w && i < 64; i++) {
    WindowView v{};
    if (!ctx.windows().snapshot(w, v)) return 0;
    if (v.parent_xid == eventWin) return w;
    w = v.parent_xid;
  }
  return 0;
}

// Stage-2 mask tests at the grab's level (DeliverOneGrabbedEvent,
// dix/events.c:4322-4350): an XI2 grab consults its xi2mask and delivers XI2
// only; a core grab consults eventMask and delivers core only.
inline bool grabWantsXI2(const PointerGrab& g, uint32_t xi2bit)   { return g.is_xi2 && (g.xi2mask & xi2bit) != 0; }
inline bool grabWantsCore(const PointerGrab& g, uint32_t corebit) { return !g.is_xi2 && (g.eventMask & corebit) != 0; }

// Core motion filter families — PointerMotion, ButtonMotion, Button1-5Motion
// for the held buttons — against a core mask (the TryClientEvents filter).
inline bool coreMaskWantsMotion(uint32_t mask, uint32_t held) {
  if (mask & x11::mask::PointerMotion) return true;
  if (held == 0) return false;
  if (mask & x11::mask::ButtonMotion) return true;
  for (int i = 0; i < 5; i++) {
    if ((held & (1u << i)) && (mask & (1u << (8 + i)))) return true;
  }
  return false;
}

// Enter/Leave for window `w` while pointer grab `g` is active — xorg
// CoreEnterLeaveEvent (dix/events.c:4716-4723) and DeviceEnterLeaveEvent
// (:4834-4840): the grab's mask applies to the grab window; with owner_events
// the grabbing client's own selection on `w` applies as well; nothing else
// is delivered, and delivery is to the grabbing client.
struct CrossingDecision {
  bool coreOk = false;
  bool xi2Ok  = false;
  int  toFd   = -1;
};
inline CrossingDecision crossingUnderGrab(XProtoContext& ctx, const PointerGrab& g,
                                          uint32_t w, bool is_enter) {
  CrossingDecision c;
  const uint32_t corebit = is_enter ? x11::mask::EnterWindow : x11::mask::LeaveWindow;
  const uint32_t xi2bit  = is_enter ? x11::xi2::kEnterMask   : x11::xi2::kLeaveMask;
  if (w == g.grabWindow) {
    c.coreOk = !g.is_xi2 && (g.eventMask & corebit) != 0;
    c.xi2Ok  =  g.is_xi2 && (g.xi2mask  & xi2bit)  != 0;
  }
  if (g.ownerEvents) {
    const WindowView* vw = ctx.window(w);
    if (vw && vw->owner_fd == g.owner_fd) {
      c.coreOk = c.coreOk || (vw->event_mask & corebit) != 0;
      c.xi2Ok  = c.xi2Ok  || (vw->xi2_mask  & xi2bit)  != 0;
    }
  }
  c.toFd = (c.coreOk || c.xi2Ok) ? g.owner_fd : -1;
  return c;
}

} // namespace x11::grabroute
