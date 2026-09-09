//
//  GrabChoreography.hpp
//  X11LowLevel
//
//  Grab activation / deactivation choreography shared by the core grab
//  requests (GrabOps.cpp) and the XI2 ones (ExtensionOps.cpp), each step
//  mirroring the xorg function named beside it (Phase B, v1.20.0.17 —
//  docs/XI2_XORG_COMPARISON.md M3 / M7 / M22).
//

#pragma once
#include <cstdint>

#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include "Core/WindowView.hpp"
#include "Core/XConstants.hpp"
#include "Ops/EventOps.hpp"
#include "Utils/WireLE.hpp"
#include "Utils/EnterLeave.hpp"    // Phase D: relation-derived crossings
#include "Utils/FocusEvents.hpp"   // Phase D: relation-derived focus events

namespace x11::grabchoreo {

// xorg GrabDevice (dix/events.c:5257-5260): `!pWin->realized` →
// GrabNotViewable.  Realized = mapped with every ancestor mapped.  Root is
// always viewable.
inline bool isViewable(XProtoContext& ctx, uint32_t win) {
  if (win == kRootWindowXid) return true;
  uint32_t cur = win;
  for (int depth = 0; cur && cur != kRootWindowXid && depth < 64; depth++) {
    WindowView v{};
    if (!ctx.windows().snapshot(cur, v)) return false;
    if (!v.mapped) return false;
    cur = v.parent_xid;
  }
  return true;
}

// The window under the pointer (xorg `sprite->win`): the deepest window that
// last received the pointer, else the host it was last over.  0 when the
// pointer is over no X window (xorg: the root window, which no client
// selects crossings on).
inline uint32_t spriteWindow(XProtoContext& ctx) {
  const auto& in = ctx.input();
  if (in.pointer_xid && ctx.window(in.pointer_xid)) return in.pointer_xid;
  if (in.last_xid && ctx.window(in.last_xid)) return in.last_xid;
  return 0;
}

// xorg DoEnterLeaveEvents(from, to, mode) as invoked by ActivatePointerGrab
// (dix/events.c:1609-1611 — NotifyGrab; from = old grab window if one was
// held, else the sprite window; to = grab window) and DeactivatePointerGrab
// (:1688-1689 — NotifyUngrab; from = grab window, to = sprite window).
// Phase D: the full relation-derived choreography (Utils/EnterLeave.hpp) —
// details, Virtual events on intermediate windows, and the grab filter on
// each delivery; nothing when from == to (dix/enterleave.c:602-603).
inline void pointerGrabCrossings(XProtoContext& ctx, EventOps& ev,
                                 uint32_t from, uint32_t to, uint8_t mode) {
  const auto& in = ctx.input();
  enterleave::doEnterLeave(ctx, ev, from, to, mode,
                           in.root_x_u, in.root_y_u, in.buttons, in.mods);
}

// xorg DoFocusEvents(from, to, mode) as invoked by ActivateKeyboardGrab
// (dix/events.c:1732-1735 — NotifyGrab; from = old grab window if one was
// held, else the focus window) and DeactivateKeyboardGrab (:1782 —
// NotifyUngrab; from = grab window, to = focus window).  Phase D: the
// relation-derived choreography (Utils/FocusEvents.hpp), core + XI2 at
// every window; a Grab/Ungrab on the focus window itself still yields the
// Nonlinear pair, as DoFocusEvents does (dix/enterleave.c:1565-1566).
inline void keyboardGrabFocusPair(XProtoContext& ctx, EventOps& ev,
                                  uint32_t from, uint32_t to, uint8_t mode) {
  focusev::doFocusEvents(ctx, ev, from, to, mode);
}

} // namespace x11::grabchoreo
