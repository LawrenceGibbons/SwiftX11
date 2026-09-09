//
//  FocusEvents.hpp
//  X11LowLevel
//
//  Focus-event choreography mirroring xorg dix/enterleave.c DoFocusEvents →
//  CoreFocusEvents / DeviceFocusEvents (Phase D, v1.20.0.25 —
//  docs/XI2_XORG_COMPARISON.md M9, M18).
//
//  Focus moves from `from` to `to`.  0 means None; kPointerRootFocus stands for
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
//  mask).
//
//  Phase G (L18): the NotifyPointer runs — FocusOut(NotifyPointer) from the
//  pointer window P up to (excluding) the window losing focus, FocusIn
//  (NotifyPointer) from below the window gaining focus down to P — follow
//  CoreFocusOutNotifyPointerEvents / CoreFocusInNotifyPointerEvents
//  (:965-1030) exactly where each transition calls them (:1037-1400), in
//  their classic single-focus form (HasFocus false, no FirstFocusChild).
//  The root window's own FocusIn/FocusOut (NotifyPointerRoot /
//  NotifyDetailNone / NonlinearVirtual) have no target here — root has no
//  WindowView, so no client can select on it — and are skipped by emitOne.
//  PointerRoot is represented by kPointerRootFocus (1), which is also the wire value
//  of PointerRoot, so GetInputFocus reports what xorg would; a RevertToParent
//  that reaches the root therefore reads as PointerRoot, whose key routing
//  (M19) matches a root-window focus anyway.
//

#pragma once
#include <cstdint>

#include "Core/XProtoContext.hpp"
#include "Core/XConstants.hpp"
#include "Ops/EventOps.hpp"
#include "Utils/EnterLeave.hpp"   // wintree helpers, notifydetail / notifymode

namespace x11::focusev {

inline bool isNoneOrPointerRoot(uint32_t w) { return w == 0 || w == kPointerRootFocus; }

inline void emitOne(XProtoContext& ctx, EventOps& ev, uint32_t w, bool is_in,
                    uint8_t mode, uint8_t detail) {
  if (isNoneOrPointerRoot(w) || !ctx.window(w)) return;
  ev.sendFocusEvent(ctx, w, is_in, mode, detail);
  ev.sendXI2FocusEvent(ctx, w, is_in, mode, detail);
}

// xorg PointerWin(): the sprite window, 0 when over no X window (root).
inline uint32_t pointerWin(XProtoContext& ctx) {
  const uint32_t p = ctx.input().pointer_xid;
  return (p && ctx.window(p)) ? p : 0;
}

// xorg DoFocusEvents(from, to, mode).
inline void doFocusEvents(XProtoContext& ctx, EventOps& ev, uint32_t from, uint32_t to, uint8_t mode) {
  using namespace notifydetail;
  const bool fromNP = isNoneOrPointerRoot(from);
  const bool toNP   = isNoneOrPointerRoot(to);
  if (from == to && mode != notifymode::kGrab && mode != notifymode::kUngrab) return;

  const uint32_t P = pointerWin(ctx);

  auto out = [&](uint32_t w, uint8_t d) { emitOne(ctx, ev, w, /*is_in=*/false, mode, d); };
  auto in  = [&](uint32_t w, uint8_t d) { emitOne(ctx, ev, w, /*is_in=*/true,  mode, d); };
  auto outChain = [&](uint32_t child, uint32_t ancestor, uint8_t d) {
    for (uint32_t w : wintree::between(ctx, child, ancestor)) out(w, d);
  };
  auto inChain = [&](uint32_t ancestor, uint32_t child, uint8_t d) {
    const std::vector<uint32_t> ws = wintree::between(ctx, child, ancestor);
    for (size_t i = ws.size(); i-- > 0;) in(ws[i], d);
  };

  // CoreFocusOutNotifyPointerEvents (:965-985): FocusOut(NotifyPointer) from
  // P up to (excluding) `parent` — or including it when `inclusive` — when P
  // is below `parent`, unless P is below or above `exclude`.
  auto outNotifyPointer = [&](uint32_t parent, uint32_t exclude, bool inclusive) {
    if (!P) return;
    const uint32_t par = wintree::normRoot(parent);
    if (!wintree::isAncestor(ctx, par, P) && !(par == P && inclusive)) return;
    if (!isNoneOrPointerRoot(exclude) &&
        (wintree::isAncestor(ctx, exclude, P) || wintree::isAncestor(ctx, P, exclude))) return;
    const uint32_t stopAt = inclusive ? wintree::parentOf(ctx, par) : par;
    for (uint32_t w = P; w && w != stopAt; w = wintree::parentOf(ctx, w)) out(w, kPointer);
  };
  // CoreFocusInNotifyPointerEvents (:1004-1030): FocusIn(NotifyPointer) from
  // below `parent` (from `parent` itself when `inclusive`) down to P.
  auto inNotifyPointer = [&](uint32_t parent, uint32_t exclude, bool inclusive) {
    const uint32_t par = wintree::normRoot(parent);
    if (!P || P == exclude || (par != P && !wintree::isAncestor(ctx, par, P))) return;
    if (!isNoneOrPointerRoot(exclude) &&
        (wintree::isAncestor(ctx, exclude, P) || wintree::isAncestor(ctx, P, exclude))) return;
    std::vector<uint32_t> chain;   // P upward, stopping at `par` (included when inclusive)
    for (uint32_t w = P; w; w = wintree::parentOf(ctx, w)) {
      if (w == par) { if (inclusive) chain.push_back(w); break; }
      chain.push_back(w);
    }
    for (size_t i = chain.size(); i-- > 0;) in(chain[i], kPointer);
  };

  if (fromNP && toNP) {                              // CoreFocusPointerRootNoneSwitch
    if (from == kPointerRootFocus && to != kPointerRootFocus) outNotifyPointer(kRootWindowXid, 0, /*inclusive=*/true);
    // root: FocusOut(PointerRoot|DetailNone), FocusIn(PointerRoot|DetailNone) — no target here
    if (to == kPointerRootFocus) inNotifyPointer(kRootWindowXid, 0, /*inclusive=*/true);
    return;
  }
  if (toNP) {                                        // CoreFocusToPointerRootOrNone
    outNotifyPointer(from, 0, false);
    out(from, kNonlinear);
    outChain(from, kRootWindowXid, kNonlinearVirtual);
    // root: FocusIn(PointerRoot | DetailNone) — no target here
    if (to == kPointerRootFocus) inNotifyPointer(kRootWindowXid, 0, /*inclusive=*/true);
    return;
  }
  if (fromNP) {                                      // CoreFocusFromPointerRootOrNone
    if (from == kPointerRootFocus) outNotifyPointer(kRootWindowXid, 0, /*inclusive=*/true);
    // root: FocusOut(PointerRoot | DetailNone), FocusIn(NonlinearVirtual) — no target here
    inChain(kRootWindowXid, to, kNonlinearVirtual);
    in(to, kNonlinear);
    inNotifyPointer(to, 0, false);
    return;
  }
  if (from == to) {                                  // Grab/Ungrab on the focus window: NonLinear(A, A)
    outNotifyPointer(from, 0, false);
    out(from, kNonlinear);
    in(to, kNonlinear);
    inNotifyPointer(to, 0, false);
    return;
  }
  if (wintree::isAncestor(ctx, to, from)) {          // CoreFocusToAncestor (:1123-1172)
    out(from, kAncestor);
    outChain(from, to, kVirtual);
    in(to, kInferior);
    inNotifyPointer(to, from, false);
  } else if (wintree::isAncestor(ctx, from, to)) {   // CoreFocusToDescendant (:1177-1225)
    outNotifyPointer(from, to, false);
    out(from, kInferior);
    inChain(from, to, kVirtual);
    in(to, kAncestor);
  } else {                                           // CoreFocusNonLinear (:1037-1120)
    const uint32_t common = wintree::commonAncestor(ctx, from, to);
    outNotifyPointer(from, 0, false);
    out(from, kNonlinear);
    outChain(from, common, kNonlinearVirtual);
    inChain(common, to, kNonlinearVirtual);
    in(to, kNonlinear);
    inNotifyPointer(to, 0, false);
  }
}

} // namespace x11::focusev
