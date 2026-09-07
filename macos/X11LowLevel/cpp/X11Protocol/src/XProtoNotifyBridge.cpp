//
//  XProtoNotifyBridge.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/20/26.
//

#include "Core/XProtoNotifyQueue.hpp"
#include "Ops/EventOps.hpp"
#include "Core/XProtoContext.hpp"   // whatever you currently have
#include "Core/WindowTable.hpp"
#include "Core/GrabTable.hpp"
#include "XProtoMotionRoute.hpp"
#include "Core/CursorRouting.hpp"
#include "Core/InputRouting.hpp"
#include "Core/XEventMask.hpp"
#include "Core/XI2EventMask.hpp"   // xi2::kMotionMask — XI2|core deliverability (M10)
#include "Core/XConstants.hpp"
#include "Utils/GrabRoute.hpp"
#include "Utils/EnterLeave.hpp"   // Phase D crossing choreography      // Phase B2: grab-time routing (xorg DeliverGrabbedEvent)
#include "Utils/DragTrace.hpp"

#include <atomic>

namespace x11 {

  // Server-wide pointers: set once on server creation, cleared when all clients disconnect.
  // g_q is eliminated — notify queue is accessed per-client via ctx.transport().notifyQueue().
  static std::atomic<EventOps*>       g_ev{nullptr};
  static std::atomic<XProtoContext*>  g_ctx{nullptr};

  extern "C" void x11_cpp_notify_init(void* ctx_ptr, void* event_ops_ptr, void* /*queue_ptr*/) {
    g_ctx.store(reinterpret_cast<XProtoContext*>(ctx_ptr), std::memory_order_release);
    g_ev.store(reinterpret_cast<EventOps*>(event_ops_ptr), std::memory_order_release);
  }

  extern "C" void x11_cpp_notify_shutdown(void) {
    g_ev.store(nullptr, std::memory_order_release);
    g_ctx.store(nullptr, std::memory_order_release);
  }

  extern "C" void x11_cpp_notify_queue(uint32_t wid, int want_cfg, int want_exp) {
    auto* ctx = g_ctx.load(std::memory_order_acquire);
    if (!ctx || !ctx->hasClient()) return;
    ctx->transport().queueNotify(wid, want_cfg != 0, want_exp != 0);
  }

  extern "C" size_t x11_cpp_notify_drain(void* out_array, size_t max_items) {
    auto* ctx = g_ctx.load(std::memory_order_acquire);
    if (!ctx || !ctx->hasClient()) return 0;
    return ctx->transport().notifyQueue().drain(
        reinterpret_cast<PendingNotify*>(out_array), max_items);
  }

  extern "C" void x11_cpp_notify_flush_one(const void* pending_notify, uint16_t seq) {
    auto* ev = g_ev.load(std::memory_order_acquire);
    if (!ev) return;
    const auto& pn = *reinterpret_cast<const PendingNotify*>(pending_notify);
    ev->flushPendingNotify(pn, seq);
  }

}


namespace x11::notify {


void postMotion(uint32_t host_xid,
                int32_t win_x, int32_t win_y,
                int32_t root_x, int32_t root_y,
                uint8_t deliver,
                uint32_t buttons, uint32_t mods)
{
  
  XProtoContext* ctx = g_ctx.load(std::memory_order_acquire);
  EventOps* ev = g_ev.load(std::memory_order_acquire);
  if (!ctx || !ev) return;

  // GlobalPointerTracker ticks (deliver=0) carry no button/modifier state
  // (sentinel 0xFFFFFFFF) and a cached window-local position.  xorg fills
  // state from the device at delivery time (Xi/exevents.c:1840-1862); use
  // the canonical InputState and derive the host-local position from the
  // cached host origin, so grab-routed XI2 motion no longer alternates
  // between real and empty state at 30 Hz (M8 in docs/XI2_XORG_COMPARISON.md).
  // Doing this BEFORE updateMotion also keeps hostOrigins self-consistent.
  constexpr uint32_t kUnknownState = 0xFFFFFFFFu;
  if (buttons == kUnknownState) buttons = ctx->input().buttons;
  if (mods    == kUnknownState) mods    = ctx->input().mods;
  if (!deliver && host_xid) {
    int32_t ox = 0, oy = 0;
    if (ctx->input().getHostOrigin(host_xid, ox, oy)) {
      win_x = root_x - ox;
      win_y = root_y - oy;
    }
  }

  // Always update InputState with global root coords (QueryPointer follows everywhere)
  ctx->input().updateMotion(host_xid,
                            win_x, win_y,
                            root_x, root_y,
                            buttons, mods);

  // Raw motion trace — counts EVERY motion that reaches postMotion, before
  // any of the early returns or routing logic.  Comparing this against the
  // `motions` count at DRAG END tells us whether motion events are being
  // dropped between Cocoa and X11 delivery.  Also captures the CURRENT
  // drag_xid so we can see whether it got silently cleared mid-drag.
  x11::drag_trace::motionRaw(host_xid, root_x, root_y, deliver, buttons,
                             ctx->input().drag_xid);

  // XI2 RawMotion is a root-level event — deliver on ANY pointer move,
  // even when deliver=0 (cursor outside X11 windows).  GlobalPointerTracker
  // fires with deliver=0 for global motion; xeyes relies on this for
  // cursor tracking across the entire screen.  Window-free: it fans out to
  // every root selector, so host_xid plays no part (v1.20.0.13).
  ev->sendXI2RawMotionEvent(*ctx);

  // Only deliver core MotionNotify when inside (or dragging/grab).
  // Exception: during an active pointer grab, X11 spec requires motion
  // events be delivered to the grab window regardless of pointer position.
  // The Cocoa "deliver" flag only reflects "pointer inside NSView" — it
  // must not gate X11 grab-routed events.
  if (!deliver) {
    // A tick with no host window at all (pointer never over an X window
    // yet, v1.20.0.24) has nothing to route to — RawMotion above was the
    // whole job.
    if (host_xid == 0) return;
    x11::PointerGrab earlyGrab{};
    if (!(ctx->grabs().getPointerGrab(earlyGrab) && earlyGrab.active)) {
      x11::drag_trace::dropped("no_deliver_no_grab");
      return;
    }
  }

  // ---- macOS drag correction ----
  // macOS's drag tracking loop routes ALL mouseDragged events to the
  // original mouseDown window. If a popup menu (override-redirect) is
  // on top, the pointer is actually over THAT window, not the reported
  // host. Use root coords to find the real popup under the pointer and
  // correct host_xid/win_x/win_y accordingly.
  //
  // ONLY override-redirect windows (menus/tooltips/combo popups) are
  // valid correction targets (v1.19.36.21).  The walk uses X11 stacking
  // order, which for NORMAL top-level windows is stale relative to what
  // the user actually sees (Cocoa owns their Z-order).  Correcting to a
  // normal window let a window that merely sits "above" in X11's stale
  // order steal motion over its whole rectangle even while visually
  // behind — field repro 2026-09-02: vlm behind the Vivado main window
  // froze menu highlighting and killed clicks over vlm's rect, and the
  // dead zone tracked vlm's position.  Cocoa already delivers events to
  // the correct normal window; only borderless popups need rescuing.
  const uint32_t originalHost = host_xid;
  {
    auto topLevels = ctx->windows().childrenInStackOrder(1); // root children
    // Search from topmost down; stop when we reach host_xid
    for (auto it = topLevels.rbegin(); it != topLevels.rend(); ++it) {
      if (*it == host_xid) break; // nothing above host contains pointer
      x11::WindowView vw{};
      if (!ctx->windows().snapshot(*it, vw)) continue;
      if (!vw.mapped) continue;
      if (!vw.override_redirect) continue; // only popups may capture
      int32_t bw = (int32_t)vw.border_width;
      if (root_x >= vw.x - bw && root_x < vw.x + (int32_t)vw.w + bw &&
          root_y >= vw.y - bw && root_y < vw.y + (int32_t)vw.h + bw) {
        host_xid = *it;
        win_x = root_x - vw.x;
        win_y = root_y - vw.y;
        x11::drag_trace::hostCorr(originalHost, *it, vw.w, vw.h, vw.x, vw.y);
        break;
      }
    }
  }
  const bool hostCorrected = (host_xid != originalHost);

  // ---- Check for active pointer grab (GrabPointer) ----
  x11::PointerGrab activeGrab{};
  const bool haveGrab = ctx->grabs().getPointerGrab(activeGrab) && activeGrab.active;

  // ---- Crossing events ----
  // xorg CheckMotion (dix/events.c:3205-3227) generates Enter/Leave on every
  // sprite-window change, grab or not; during a grab CoreEnterLeaveEvent /
  // DeviceEnterLeaveEvent deliver them only per the grab (:4716-4723,
  // 4834-4840) — the grab window with the grab mask, or the grabbing client's
  // own windows when owner_events.  The old gate skipped crossings entirely
  // during an implicit drag, so a pressed widget dragged out never saw its
  // Leave (G-7 / C-10).  Otherwise both levels, each gated only by its own
  // mask (DoEnterLeaveEvents, dix/enterleave.c:595-608 — R1).
  // The sprite window (xorg sprite->win): the deepest mapped window under
  // the pointer on this host.  Used for crossings and for the `child` field
  // of the motion event (FixUpEventFromWindow).
  uint32_t spriteWin = pickDeepestMappedWindowAtHostPoint(*ctx, host_xid, win_x, win_y);
  if (!spriteWin) spriteWin = host_xid;

  {
    const uint32_t under = spriteWin;

    const uint32_t prev = ctx->input().pointer_xid;
    if (under != prev) {
      // Phase D (M15): xorg DoEnterLeaveEvents(prev, under) — details from
      // the window relation, Virtual events on the windows between, the
      // grab filter applied per delivery (Utils/EnterLeave.hpp).
      x11::enterleave::doEnterLeave(*ctx, *ev, prev, under, x11::notifymode::kNormal,
                                    root_x, root_y, buttons, mods);
      ctx->input().pointer_xid = under;
    }
  }

  // Route motion: an active grab decides delivery (xorg ProcessDeviceEvent
  // consults the grab before normal delivery, Xi/exevents.c:1938-1940).
  // drag_xid is our internal button-drag tracker; since v1.20.0.18 a press
  // also records a real implicit grab, so the grab branch handles drags.
  x11::drag_trace::route(ctx->input().drag_xid, haveGrab, hostCorrected,
                         activeGrab.grabWindow, activeGrab.eventMask,
                         activeGrab.ownerEvents);

  // X11 motion-event delivery is gated by THREE families of mask bits:
  //   bit 6  (0x0040) PointerMotionMask     — all motion
  //   bit 13 (0x2000) ButtonMotionMask      — motion while ANY button held
  //   bits 8-12      Button1-5MotionMask   — motion while a specific button held
  //
  // Before v1.19.35.62 we only honoured PointerMotionMask, so an active
  // pointer grab installed by AWT's XDND code with mask
  //   0x200C = ButtonPress | ButtonRelease | ButtonMotion
  // dropped every drag motion event into the no-target sink and the
  // hw_ila drag silently died.  Now we accept any motion bit relevant to
  // the currently-held buttons.
  auto grabWantsMotion = [&](uint16_t mask, uint32_t held) -> bool {
    if (mask & (1u << 6))  return true;                    // PointerMotion
    if (held == 0)         return false;                   // nothing else applies
    if (mask & (1u << 13)) return true;                    // ButtonMotion (any)
    // Per-button: held bit N → mask bit (8 + N)
    for (int i = 0; i < 5; i++) {
      if ((held & (1u << i)) && (mask & (1u << (8 + i)))) return true;
    }
    return false;
  };
  const uint32_t heldButtons = ctx->input().buttons;

  uint32_t target  = 0;  // EXPLICITLY initialise — must never be undefined
  bool     viaGrab = false;
  int      toFd    = -1;
  if (haveGrab) {
    // xorg DeliverGrabbedEvent (dix/events.c:4399-4434): with owner_events,
    // normal delivery — but only to the grabbing client's windows (Xt popup
    // menus use owner_events=True so MotionNotify reaches the SimpleMenu
    // child); otherwise, or when that finds nobody, the grab window at the
    // grab's level with the grab's mask, addressed to the grabbing client —
    // which is how a root-window grab (AWT's XDND) reaches AWT without the
    // old drag_xid fallback.
    uint32_t normal = 0;
    if (activeGrab.ownerEvents) {
      normal = pick_motion_target(*ctx, host_xid, win_x, win_y);
      if (!normal && activeGrab.grabWindow != host_xid) {
        // macOS routes drag events to the original mouseDown window while
        // the pointer is over the popup (a different host): retry within the
        // grab window's subtree using root coords translated to its space.
        x11::WindowView gv{};
        if (ctx->windows().snapshot(activeGrab.grabWindow, gv)) {
          normal = pick_motion_target(*ctx, activeGrab.grabWindow,
                                      root_x - gv.x, root_y - gv.y);
        }
      }
    }
    const auto d = x11::grabroute::route(*ctx, activeGrab, normal);
    target  = d.target;
    viaGrab = d.viaGrab;
    if (viaGrab) toFd = activeGrab.owner_fd;
  } else if (ctx->input().drag_xid && !hostCorrected) {
    // No active grab, but button drag in progress AND pointer is still
    // over the same host (e.g., scrollbar drag within xterm).
    // When hostCorrected is true, the pointer moved to a different
    // top-level window (popup menu), so ignore drag_xid and route
    // to the actual window under the pointer.
    //
    // Mask filtering (review §2.6, reinstated v1.19.36.19): the implicit
    // grab only reports motion the grab window selected for; if it
    // selected none, climb the parent chain like ButtonPress propagation.
    // (The .18 revert blamed untrustworthy stored masks, but field data
    // disproved that — xterm's scrollbar mask is 0xA70F with all motion
    // bits present; the .17 "thumb regression" was a testing slip.)
    target = ctx->input().drag_xid;
    {
      const uint32_t held = ctx->input().buttons;
      auto wantsMotionAt = [&](uint32_t xid) -> bool {
        const x11::WindowView* vw = ctx->window(xid);
        // xorg EventIsDeliverable: the window's XI2 selection OR its core
        // mask (M10 in docs/XI2_XORG_COMPARISON.md).
        return vw && (grabWantsMotion((uint16_t)vw->event_mask, held) ||
                      (vw->xi2_mask & x11::xi2::kMotionMask) != 0);
      };
      if (!wantsMotionAt(target)) {
        uint32_t cur = target;
        uint32_t found = 0;
        for (int hop = 0; hop < 64 && cur; hop++) {
          x11::WindowView vw{};
          if (!ctx->windows().snapshot(cur, vw)) break;
          // do_not_propagate_mask fences motion propagation too (§2.8)
          if (vw.do_not_propagate_mask & (1u << 6)) break;
          cur = vw.parent_xid;
          if (cur && wantsMotionAt(cur)) { found = cur; break; }
        }
        if (found) target = found;
        // Nobody wants motion → keep drag_xid (harmless; masks drop it).
      }
    }
  } else {
    // Normal case: route based on WINDOW-LOCAL coords
    target = pick_motion_target(*ctx, host_xid, win_x, win_y);
  }


  if (!target) {
    x11::drag_trace::dropped("no_target");
    return;
  }

  // The cursor is applied to the *host* (Cocoa window), but chosen from the *effective target*.
  uint32_t host = host_xid;
  if (!host) host = ctx->input().focus_host;
  if (!host) host = ctx->windows().topLevelAncestorOf(target); // last-resort

  const uint32_t cursorTarget = ctx->input().routePointer(target); // respects drag_xid/pointer_xid/focus_xid
  maybeApplyCursor(*ctx, host, cursorTarget);

  // Drag diagnostic — ticks each motion event delivered during an
  // active button-down → button-up bracket.  No-op when X11_TRACE_DRAG
  // is disabled.  We record:
  //   - the actual delivery target (vs. just drag_xid)
  //   - the root_x/root_y the wire event will carry
  //   - the buttons + mods we're about to feed sendMotionNotify
  // so we can verify the motion event's `state` field will include
  // Button1Mask (X11 wire bit 8) once toX11State() runs inside
  // sendMotionNotify.  AWT only treats motion as drag-eligible when
  // Button1Mask is set; the hw_ila drag fails despite plenty of
  // motion events arriving, so this is the next thing to verify.
  x11::drag_trace::motion(target, root_x, root_y, buttons, mods);

  // child = the child of the event window on the sprite path (xorg
  // FixUpEventFromWindow, calcChild=TRUE) — for a root-window grab the
  // toplevel under the pointer, which AWT's XDND reads as `subwindow`.
  const uint32_t child = x11::grabroute::childOnSpritePath(*ctx, target, spriteWin);

  if (viaGrab) {
    // DeliverOneGrabbedEvent (dix/events.c:4322-4350): the grab's level only,
    // filtered by the grab's own mask — XI_Motion for an XI2 grab, the core
    // motion families for the held buttons for a core grab.
    if (activeGrab.is_xi2) {
      if (activeGrab.xi2mask & x11::xi2::kMotionMask)
        (void)ev->sendXI2MotionEvent(*ctx, target, root_x, root_y, buttons, mods, /*force=*/true, toFd, child);
    } else if (grabWantsMotion(activeGrab.eventMask, heldButtons)) {
      ev->sendMotionNotify(*ctx, target, root_x, root_y, buttons, mods, toFd, child);
    }
    return;
  }

  // Send MotionNotify with ROOT coords (root_x/root_y).
  // xorg DeliverDeviceEvents: XI2 first; if the window's own selection consumes
  // it, the core MotionNotify is suppressed (no double delivery).
  if (!ev->sendXI2MotionEvent(*ctx, target, root_x, root_y, buttons, mods, false, -1, child)) {
    ev->sendMotionNotify(*ctx, target, root_x, root_y, buttons, mods, -1, child);
  }
}
  
void postButtonLegacy(uint32_t xid, int is_press, int32_t x_px, int32_t y_px, uint32_t buttons, uint32_t mods)
{
  XProtoContext* ctx = g_ctx.load(std::memory_order_acquire);
  EventOps* ev = g_ev.load(std::memory_order_acquire);
  if (!ctx || !ev) return;

  // TODO: implement ButtonPress/Release later.
  (void)is_press; (void)x_px; (void)y_px; (void)buttons; (void)mods;
}

  
} // namespace x11::notify


namespace x11 {
  
  uint32_t pick_motion_target(XProtoContext& ctx,
                              uint32_t host_xid,
                              int32_t x_px,
                              int32_t y_px)
  {
    
    
    // If host window itself doesn't exist, bail.
    WindowView host{};
    if (!ctx.windows().snapshot(host_xid, host)) return 0;
    
    // Gather subtree (children, grandchildren, ...)
    std::vector<uint32_t> nodes = ctx.windows().descendantsOf(host_xid);
    
    // Include the host window itself as a candidate.
    nodes.push_back(host_xid);
    
    // Pick the deepest window that contains the point, preferring children over parents.
    // We do this by tracking "depth": number of ancestors between host and window.
    uint32_t best = host_xid;
    int bestDepth = -1;
    int16_t bestX = 0, bestY = 0;
    
    // Pre-snapshot host geometry for relative coords.
    // Your Swift coords are in the host view’s coordinate system, so host is origin.
    // We'll treat host local coords = (x_px, y_px).
    for (uint32_t xid : nodes) {
      WindowView vw{};
      if (!ctx.windows().snapshot(xid, vw)) continue;
      if (!vw.mapped) continue;
      
      // Compute depth relative to host by walking parents (using snapshots)
      int depth = 0;
      uint32_t p = vw.parent_xid;
      while (p && p != host_xid) {
        WindowView pv{};
        if (!ctx.windows().snapshot(p, pv)) { depth = -1; break; }
        p = pv.parent_xid;
        depth++;
        if (depth > 64) { depth = -1; break; } // safety
      }
      if (depth < 0) continue;
      if (xid != host_xid && p != host_xid) continue; // not actually in subtree chain
      
      // Convert point into this window's parent coordinate space and test containment.
      // For now: assume all child coords are relative to host (not strictly true).
      // Better: walk down and subtract ancestor offsets. We'll do the correct version below.
    }
    
    // Correct version: compute local coords by subtracting ancestor offsets.
    auto contains = [&](uint32_t xid, WindowView& vw, int32_t& outLocalX, int32_t& outLocalY, int& outDepth) -> bool {
      outLocalX = x_px;
      outLocalY = y_px;
      outDepth = 0;
      
      uint32_t cur = xid;
      while (cur && cur != host_xid) {
        WindowView cv{};
        if (!ctx.windows().snapshot(cur, cv)) return false;
        // subtract this window's offset in its parent (including border_width)
        outLocalX -= (cv.x + cv.border_width);
        outLocalY -= (cv.y + cv.border_width);
        cur = cv.parent_xid;
        outDepth++;
        if (outDepth > 64) return false;
      }
      if (cur != host_xid && xid != host_xid) return false;
      
      // Now outLocalX/outLocalY are in xid's local (drawable) coords.
      // Include border region in hit test (border is part of the window's footprint).
      const int32_t bw_i = (int32_t)vw.border_width;
      return (outLocalX >= -bw_i && outLocalY >= -bw_i &&
              outLocalX < (int32_t)vw.w + bw_i &&
              outLocalY < (int32_t)vw.h + bw_i);
    };
    
    for (uint32_t xid : nodes) {
      WindowView vw{};
      if (!ctx.windows().snapshot(xid, vw)) continue;
      if (!vw.mapped) continue;
      
      int32_t lx = 0, ly = 0;
      int depth = 0;
      if (!contains(xid, vw, lx, ly, depth)) continue;

      // SHAPE extension: check input/bounding shape containment
      if ((vw.input_shaped || vw.bounding_shaped) &&
          !ctx.windows().isInShapeRegion(xid, (int16_t)lx, (int16_t)ly)) continue;

      // Prefer deepest window.
      if (depth > bestDepth) {
        best = xid;
        bestDepth = depth;
        bestX = (int16_t)lx;
        bestY = (int16_t)ly;
      }
    }
    
    // Build motion mask: PointerMotionMask always, plus button-specific
    // masks when buttons are pressed (e.g., ButtonMotionMask for Xaw
    // SimpleMenu highlighting via <BtnMotion> translation).
    uint32_t motionMask = x11::mask::PointerMotion; // bit 6
    uint32_t btns = ctx.input().buttons;
    if (btns) {
      motionMask |= x11::mask::ButtonMotion; // bit 13: any button
      for (int b = 0; b < 5; b++) {
        if (btns & (1u << b))
          motionMask |= (1u << (8 + b)); // Button{1-5}MotionMask
      }
    }

    // Walk up until you find a motion listener at EITHER level — xorg
    // EventIsDeliverable (dix/events.c:2747-2773) accepts the window's XI2
    // union or its core mask.  XI2 has only XI_Motion (no button-motion
    // families).  A core-only walk fell off the top for a window tree that
    // selected motion solely via XISelectEvents (M10).
    uint32_t cur = best;
    while (cur) {
      WindowView vw{};
      if (!ctx.windows().snapshot(cur, vw)) break;
      if ((vw.event_mask & motionMask) || (vw.xi2_mask & xi2::kMotionMask)) return cur;
      cur = vw.parent_xid;
    }

    return 0;
  }
  
}
