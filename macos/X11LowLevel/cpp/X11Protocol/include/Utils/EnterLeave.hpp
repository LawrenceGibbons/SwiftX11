//
//  EnterLeave.hpp
//  X11LowLevel
//
//  Crossing-event choreography mirroring xorg dix/enterleave.c
//  DoEnterLeaveEvents → CoreEnterLeaveEvents / DeviceEnterLeaveEvents
//  (Phase D, v1.20.0.25 — docs/XI2_XORG_COMPARISON.md M15, M16, M18).
//
//  The pointer moves from window `from` to window `to` (0 = the root window,
//  i.e. over no X window).  The events and their `detail` follow from the
//  window relation:
//
//    to is a descendant of from:  Leave(from, Inferior)
//                                 Enter(each window between, Virtual)   top-down
//                                 Enter(to, Ancestor)
//    from is a descendant of to:  Leave(from, Ancestor)
//                                 Leave(each window between, Virtual)   bottom-up
//                                 Enter(to, Inferior)
//    otherwise (common ancestor X): Leave(from, Nonlinear)
//                                 Leave(from → X, NonlinearVirtual)     bottom-up
//                                 Enter(X → to, NonlinearVirtual)       top-down
//                                 Enter(to, Nonlinear)
//
//  Endpoint events carry child = None; the Virtual ones carry the window one
//  step down the path (dix/enterleave.c:229-340, 346-535, 555-586).  Both
//  the core and the XI2 event go out for every window, each gated only by
//  its own selection (DoEnterLeaveEvents :595-608); during a pointer grab the
//  grab decides (CoreEnterLeaveEvent, dix/events.c:4716-4723).  The root
//  window has no WindowView here and is skipped.
//

#pragma once
#include <cstdint>
#include <vector>

#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include "Core/WindowView.hpp"
#include "Core/XConstants.hpp"
#include "Core/GrabTable.hpp"
#include "Ops/EventOps.hpp"
#include "Utils/GrabRoute.hpp"

namespace x11 {

// Crossing / focus `detail` values (X11 core protocol)
namespace notifydetail {
constexpr uint8_t kAncestor         = 0;
constexpr uint8_t kVirtual          = 1;
constexpr uint8_t kInferior         = 2;
constexpr uint8_t kNonlinear        = 3;
constexpr uint8_t kNonlinearVirtual = 4;
constexpr uint8_t kPointer          = 5;
constexpr uint8_t kPointerRoot      = 6;
constexpr uint8_t kDetailNone       = 7;
}

// Crossing / focus `mode` values
namespace notifymode {
constexpr uint8_t kNormal       = 0;
constexpr uint8_t kGrab         = 1;
constexpr uint8_t kUngrab       = 2;
constexpr uint8_t kWhileGrabbed = 3;
}

// ---------------------------------------------------------------------------
// Window-tree helpers shared by the crossing and focus choreography.
// 0 and kRootWindowXid both mean the root; every mapped X window descends from it.
// ---------------------------------------------------------------------------
namespace wintree {

inline uint32_t normRoot(uint32_t w) { return w == 0 ? kRootWindowXid : w; }

// Parent of `w`; kRootWindowXid for a toplevel; 0 for the root or an unknown window.
inline uint32_t parentOf(XProtoContext& ctx, uint32_t w) {
  if (w == 0 || w == kRootWindowXid) return 0;
  WindowView v{};
  if (!ctx.windows().snapshot(w, v)) return 0;
  return v.parent_xid ? v.parent_xid : kRootWindowXid;
}

// xorg IsParent(a, b): `a` is a proper ancestor of `b`.
inline bool isAncestor(XProtoContext& ctx, uint32_t a, uint32_t b) {
  a = normRoot(a); b = normRoot(b);
  if (a == b || b == kRootWindowXid) return false;
  if (a == kRootWindowXid) return true;
  uint32_t w = parentOf(ctx, b);
  for (int i = 0; w && i < 64; i++) {
    if (w == a) return true;
    if (w == kRootWindowXid) return false;
    w = parentOf(ctx, w);
  }
  return false;
}

// xorg CommonAncestor(a, b): the first ancestor of `b` that is also an
// ancestor of `a` (root at worst).
inline uint32_t commonAncestor(XProtoContext& ctx, uint32_t a, uint32_t b) {
  a = normRoot(a); b = normRoot(b);
  uint32_t p = parentOf(ctx, b);
  for (int i = 0; p && i < 64; i++) {
    if (p == kRootWindowXid || isAncestor(ctx, p, a)) return p;
    p = parentOf(ctx, p);
  }
  return kRootWindowXid;
}

// Windows strictly between `child` and its ancestor `ancestor`, ordered from
// the child upward (the order xorg's *LeaveNotifies use; reverse for the
// *EnterNotifies recursion, which emits top-down).
inline std::vector<uint32_t> between(XProtoContext& ctx, uint32_t child, uint32_t ancestor) {
  std::vector<uint32_t> out;
  ancestor = normRoot(ancestor);
  uint32_t w = parentOf(ctx, normRoot(child));
  for (int i = 0; w && w != ancestor && i < 64; i++) {
    if (w == kRootWindowXid) break;
    out.push_back(w);
    w = parentOf(ctx, w);
  }
  return out;
}

} // namespace wintree

namespace enterleave {

// One crossing to `w`, through the active pointer grab if any.  The root is a
// real window since R1 (Phase 3): its Enter(Inferior)/Leave(Inferior) and the
// Virtual chains that end at it reach root selectors (xev -root, a WM).
inline void emitOne(XProtoContext& ctx, EventOps& ev, uint32_t w, bool is_enter,
                    uint8_t mode, uint8_t detail, uint32_t child,
                    int32_t rx, int32_t ry, uint32_t buttons, uint32_t mods) {
  if (w == 0 || !ctx.window(w)) return;
  PointerGrab g{};
  if (ctx.grabs().getPointerGrab(g) && g.active) {
    const auto cd = grabroute::crossingUnderGrab(ctx, g, w, is_enter);
    if (cd.coreOk) ev.sendCrossingEvent(ctx, w, is_enter, rx, ry, buttons, mods, mode, cd.toFd, detail, child);
    if (cd.xi2Ok)  (void)ev.sendXI2CrossingEvent(ctx, w, is_enter, rx, ry, buttons, mods, mode,
                                                 /*force=*/true, cd.toFd, detail, child);
    return;
  }
  ev.sendCrossingEvent(ctx, w, is_enter, rx, ry, buttons, mods, mode, -1, detail, child);
  (void)ev.sendXI2CrossingEvent(ctx, w, is_enter, rx, ry, buttons, mods, mode, false, -1, detail, child);
}

// xorg DoEnterLeaveEvents(from, to, mode).
inline void doEnterLeave(XProtoContext& ctx, EventOps& ev, uint32_t from, uint32_t to, uint8_t mode,
                         int32_t rx, int32_t ry, uint32_t buttons, uint32_t mods) {
  using namespace notifydetail;
  from = wintree::normRoot(from);
  to   = wintree::normRoot(to);
  if (from == to) return;

  auto leave = [&](uint32_t w, uint8_t detail, uint32_t child) {
    emitOne(ctx, ev, w, /*is_enter=*/false, mode, detail, child, rx, ry, buttons, mods);
  };
  auto enter = [&](uint32_t w, uint8_t detail, uint32_t child) {
    emitOne(ctx, ev, w, /*is_enter=*/true, mode, detail, child, rx, ry, buttons, mods);
  };
  // *LeaveNotifies: bottom-up, child = the window one step below.
  auto leaveChain = [&](uint32_t child, uint32_t ancestor, uint8_t detail) {
    uint32_t below = child;
    for (uint32_t w : wintree::between(ctx, child, ancestor)) { leave(w, detail, below); below = w; }
  };
  // *EnterNotifies: top-down, child = the window one step below.
  auto enterChain = [&](uint32_t ancestor, uint32_t child, uint8_t detail) {
    const std::vector<uint32_t> ws = wintree::between(ctx, child, ancestor);
    for (size_t i = ws.size(); i-- > 0;) {
      const uint32_t below = (i == 0) ? child : ws[i - 1];
      enter(ws[i], detail, below);
    }
  };

  if (wintree::isAncestor(ctx, from, to)) {          // CoreEnterLeaveToDescendant
    leave(from, kInferior, 0);
    enterChain(from, to, kVirtual);
    enter(to, kAncestor, 0);
  } else if (wintree::isAncestor(ctx, to, from)) {   // CoreEnterLeaveToAncestor
    leave(from, kAncestor, 0);
    leaveChain(from, to, kVirtual);
    enter(to, kInferior, 0);
  } else {                                           // CoreEnterLeaveNonLinear
    const uint32_t common = wintree::commonAncestor(ctx, from, to);
    leave(from, kNonlinear, 0);
    leaveChain(from, common, kNonlinearVirtual);
    enterChain(common, to, kNonlinearVirtual);
    enter(to, kNonlinear, 0);
  }
}

} // namespace enterleave
} // namespace x11
