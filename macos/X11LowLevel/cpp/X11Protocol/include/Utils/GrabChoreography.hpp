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

namespace x11::grabchoreo {

// xorg GrabDevice (dix/events.c:5257-5260): `!pWin->realized` →
// GrabNotViewable.  Realized = mapped with every ancestor mapped.  Root is
// always viewable.
inline bool isViewable(XProtoContext& ctx, uint32_t win) {
  if (win == kRootXid) return true;
  uint32_t cur = win;
  for (int depth = 0; cur && cur != kRootXid && depth < 64; depth++) {
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
// (:1688-1689 — NotifyUngrab; from = grab window, to = sprite window).  Both
// the core and the XI2 crossing are sent, each gated only by its own mask
// (dix/enterleave.c:595-608); nothing when from == to (:602-603).
inline void pointerGrabCrossings(XProtoContext& ctx, EventOps& ev,
                                 uint32_t from, uint32_t to, uint8_t mode) {
  if (from == to) return;
  const auto& in = ctx.input();
  if (from && ctx.window(from)) {
    ev.sendCrossingEvent(ctx, from, /*is_enter=*/false,
                         in.root_x_u, in.root_y_u, in.buttons, in.mods, mode);
    (void)ev.sendXI2CrossingEvent(ctx, from, /*is_enter=*/false,
                                  in.root_x_u, in.root_y_u, in.buttons, in.mods, mode);
  }
  if (to && ctx.window(to)) {
    ev.sendCrossingEvent(ctx, to, /*is_enter=*/true,
                         in.root_x_u, in.root_y_u, in.buttons, in.mods, mode);
    (void)ev.sendXI2CrossingEvent(ctx, to, /*is_enter=*/true,
                                  in.root_x_u, in.root_y_u, in.buttons, in.mods, mode);
  }
}

// xorg DoFocusEvents(from, to, mode) as invoked by ActivateKeyboardGrab
// (dix/events.c:1732-1735 — NotifyGrab; from = old grab window if one was
// held, else the focus window) and DeactivateKeyboardGrab (:1782 —
// NotifyUngrab; from = grab window, to = focus window).  Core FocusOut /
// FocusIn plus the XI2 twins (dix/enterleave.c:1560-1570); nothing when
// from == to.  Detail is Nonlinear for the toplevel↔toplevel transitions our
// grabs produce (M9 will compute Ancestor/Inferior for nested windows).
inline void keyboardGrabFocusPair(XProtoContext& ctx, EventOps& ev,
                                  uint32_t from, uint32_t to, uint8_t mode) {
  if (from == to) return;
  auto sendCore = [&](uint32_t wid, bool is_in) {
    uint8_t e[32] = {};
    e[0] = is_in ? 9 : 10;   // FocusIn / FocusOut
    e[1] = 3;                // detail = NotifyNonlinear
    wire::wr16_le(e + 2, ctx.transport().lastSeq());
    wire::wr32_le(e + 4, wid);
    e[8] = mode;             // 1 = NotifyGrab, 2 = NotifyUngrab
    (void)ctx.transport().sendEvent32(wid, e);
  };
  if (from && ctx.window(from)) {
    sendCore(from, false);
    ev.sendXI2FocusEvent(ctx, from, /*is_in=*/false, mode, /*detail=*/3);
  }
  if (to && ctx.window(to)) {
    sendCore(to, true);
    ev.sendXI2FocusEvent(ctx, to, /*is_in=*/true, mode, /*detail=*/3);
  }
}

} // namespace x11::grabchoreo
