//
//  FocusEvents.hpp
//  X11LowLevel
//
//  Focus-event choreography mirroring xorg dix/enterleave.c DoFocusEvents →
//  CoreFocusEvents / DeviceFocusEvents (Phase D, v1.20.0.25 —
//  docs/XI2_XORG_COMPARISON.md M9, M18).
//
//  Focus moves from `from` to `to`.  0 means None; kRootXid stands for
//  PointerRoot (SetInputFocus(PointerRoot) and RevertToPointerRoot store the
//  root as the focus).  Details from the window relation, exactly as for
//  crossings (dix/enterleave.c:1403-1425 core, 1428-1550 XI2):
//
//    to/from None or PointerRoot ↔ window: Nonlinear on the window, with
//        NonlinearVirtual on its ancestors (root itself is skipped here)
//    to is an ancestor of from:  FocusOut(from, Ancestor), FocusOut(between,
//        Virtual) bottom-up, FocusIn(to, Inferior)
//    from is an ancestor of to:  FocusOut(from, Inferior), FocusIn(between,
//        Virtual) top-down, FocusIn(to, Ancestor)
//    otherwise: FocusOut(from, Nonlinear), FocusOut(from → common,
//        NonlinearVirtual), FocusIn(common → to, NonlinearVirtual),
//        FocusIn(to, Nonlinear)
//
//  from == to yields nothing except under NotifyGrab / NotifyUngrab, where
//  xorg still emits the Nonlinear pair (DoFocusEvents :1565-1566).  Every
//  event goes out at both levels, each gated by its own selection (M18: the
//  core FocusIn/FocusOut is delivered only to clients selecting FocusChange,
//  as DeliverEventsToWindow does; the WM-emulation sites used to bypass the
//  mask).  NotifyPointer / NotifyPointerRoot / NotifyDetailNone events (the
//  sprite-window and root variants) are not modelled.
//

#pragma once
#include <cstdint>

#include "Core/XProtoContext.hpp"
#include "Core/XConstants.hpp"
#include "Ops/EventOps.hpp"
#include "Utils/EnterLeave.hpp"   // wintree helpers, notifydetail / notifymode

namespace x11::focusev {

inline bool isNoneOrPointerRoot(uint32_t w) { return w == 0 || w == kRootXid; }

inline void emitOne(XProtoContext& ctx, EventOps& ev, uint32_t w, bool is_in,
                    uint8_t mode, uint8_t detail) {
  if (isNoneOrPointerRoot(w) || !ctx.window(w)) return;
  ev.sendFocusEvent(ctx, w, is_in, mode, detail);
  ev.sendXI2FocusEvent(ctx, w, is_in, mode, detail);
}

// xorg DoFocusEvents(from, to, mode).
inline void doFocusEvents(XProtoContext& ctx, EventOps& ev, uint32_t from, uint32_t to, uint8_t mode) {
  using namespace notifydetail;
  const bool fromNP = isNoneOrPointerRoot(from);
  const bool toNP   = isNoneOrPointerRoot(to);
  if (from == to && mode != notifymode::kGrab && mode != notifymode::kUngrab) return;
  if (fromNP && toNP) return;   // only root-window events, which have no target here

  auto out = [&](uint32_t w, uint8_t d) { emitOne(ctx, ev, w, /*is_in=*/false, mode, d); };
  auto in  = [&](uint32_t w, uint8_t d) { emitOne(ctx, ev, w, /*is_in=*/true,  mode, d); };
  auto outChain = [&](uint32_t child, uint32_t ancestor, uint8_t d) {
    for (uint32_t w : wintree::between(ctx, child, ancestor)) out(w, d);
  };
  auto inChain = [&](uint32_t ancestor, uint32_t child, uint8_t d) {
    const std::vector<uint32_t> ws = wintree::between(ctx, child, ancestor);
    for (size_t i = ws.size(); i-- > 0;) in(ws[i], d);
  };

  if (toNP) {                                        // CoreFocusToPointerRootOrNone
    out(from, kNonlinear);
    outChain(from, kRootXid, kNonlinearVirtual);
    return;
  }
  if (fromNP) {                                      // CoreFocusFromPointerRootOrNone
    inChain(kRootXid, to, kNonlinearVirtual);
    in(to, kNonlinear);
    return;
  }
  if (from == to) {                                  // Grab/Ungrab on the focus window itself
    out(from, kNonlinear);
    in(to, kNonlinear);
    return;
  }
  if (wintree::isAncestor(ctx, to, from)) {          // CoreFocusToAncestor
    out(from, kAncestor);
    outChain(from, to, kVirtual);
    in(to, kInferior);
  } else if (wintree::isAncestor(ctx, from, to)) {   // CoreFocusToDescendant
    out(from, kInferior);
    inChain(from, to, kVirtual);
    in(to, kAncestor);
  } else {                                           // CoreFocusNonLinear
    const uint32_t common = wintree::commonAncestor(ctx, from, to);
    out(from, kNonlinear);
    outChain(from, common, kNonlinearVirtual);
    inChain(common, to, kNonlinearVirtual);
    in(to, kNonlinear);
  }
}

} // namespace x11::focusev
