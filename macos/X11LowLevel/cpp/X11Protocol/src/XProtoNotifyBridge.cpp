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

  // Always update InputState with global root coords (QueryPointer follows everywhere)
  ctx->input().updateMotion(host_xid,
                            win_x, win_y,
                            root_x, root_y,
                            buttons, mods);

  // Only deliver MotionNotify when inside (or dragging/grab)
  if (!deliver) return;

  // ---- macOS drag correction ----
  // macOS's drag tracking loop routes ALL mouseDragged events to the
  // original mouseDown window. If a popup menu (override-redirect) is
  // on top, the pointer is actually over THAT window, not the reported
  // host. Use root coords to find the real top-level window under the
  // pointer and correct host_xid/win_x/win_y accordingly.
  const uint32_t originalHost = host_xid;
  {
    auto topLevels = ctx->windows().childrenInStackOrder(1); // root children
    // Search from topmost down; stop when we reach host_xid
    for (auto it = topLevels.rbegin(); it != topLevels.rend(); ++it) {
      if (*it == host_xid) break; // nothing above host contains pointer
      x11::WindowView vw{};
      if (!ctx->windows().snapshot(*it, vw)) continue;
      if (!vw.mapped) continue;
      int32_t bw = (int32_t)vw.border_width;
      if (root_x >= vw.x - bw && root_x < vw.x + (int32_t)vw.w + bw &&
          root_y >= vw.y - bw && root_y < vw.y + (int32_t)vw.h + bw) {
        host_xid = *it;
        win_x = root_x - vw.x;
        win_y = root_y - vw.y;
        break;
      }
    }
  }
  const bool hostCorrected = (host_xid != originalHost);
#ifndef NDEBUG
  if (hostCorrected) {
    x11::WindowView corrVw{};
    ctx->windows().snapshot(host_xid, corrVw);
    fprintf(stderr,
            "[MOTION_CORRECT] orig=0x%08X → corrected=0x%08X root=(%d,%d) vw=(%d,%d,%dx%d) win=(%d,%d)\n",
            (unsigned)originalHost, (unsigned)host_xid,
            (int)root_x, (int)root_y,
            (int)corrVw.x, (int)corrVw.y, (int)corrVw.w, (int)corrVw.h,
            (int)win_x, (int)win_y);
  }
#endif

  // ---- Check for active pointer grab (GrabPointer) ----
  x11::PointerGrab activeGrab{};
  const bool haveGrab = ctx->grabs().getPointerGrab(activeGrab) && activeGrab.active;

  // ---- Child-to-child crossing events ----
  // Pick the deepest mapped child under the pointer and generate
  // EnterNotify/LeaveNotify if the pointer moved to a different window.
  // Xaw Command widgets rely on these for hover highlighting.
  //
  // Generate crossing events when:
  //   - No drag is active (normal mouse movement), OR
  //   - An active GrabPointer exists (X11 spec: crossing events still
  //     generated during grabs — needed for menu item highlighting), OR
  //   - The host was corrected (pointer moved to a different top-level
  //     window, e.g., from xterm to popup menu during button drag)
  if (ctx->input().drag_xid == 0 || haveGrab || hostCorrected) {
    uint32_t under = pickDeepestMappedWindowAtHostPoint(*ctx, host_xid,
                                                        win_x, win_y);
    if (!under) under = host_xid;

    const uint32_t prev = ctx->input().pointer_xid;
    if (under != prev) {
      if (prev != 0) {
        ev->sendCrossingEvent(*ctx, prev, /*is_enter=*/false,
                              root_x, root_y, buttons, mods);
      }
      ev->sendCrossingEvent(*ctx, under, /*is_enter=*/true,
                            root_x, root_y, buttons, mods);
      ctx->input().pointer_xid = under;
    }
  }

  // Route motion: active grab takes priority over drag_xid.
  // X11 spec: GrabPointer controls ALL event routing while active.
  // drag_xid is our internal button-drag tracker; it must NOT override
  // an active grab (e.g., popup menu's GrabPointer with ownerEvents=True
  // needs to route to SimpleMenu child, not the original click target).
  uint32_t target;
  if (haveGrab) {
    // Active pointer grab (GrabPointer / activated passive grab).
    //
    // X11 spec for owner_events:
    //   True  → events go to the window they'd normally go to (if that
    //           window selected the event). Only fall back to grab window
    //           if no normal target wants the event.
    //   False → all events go to the grab window filtered by eventMask.
    //
    // Xt popup menus use owner_events=True so that MotionNotify reaches
    // the SimpleMenu child (which needs coords for item highlighting).
    if (activeGrab.ownerEvents) {
      // Try normal routing within the current host first
      target = pick_motion_target(*ctx, host_xid, win_x, win_y);
      if (!target && activeGrab.grabWindow != host_xid) {
        // Current host didn't contain the event target. This happens when
        // macOS routes drag events to the original mouseDown window (xterm)
        // while the pointer is actually over the popup menu (different host).
        // Try picking within the grab window's subtree using root coords
        // translated to the grab window's local coordinate space.
        x11::WindowView gv{};
        if (ctx->windows().snapshot(activeGrab.grabWindow, gv)) {
          // For a top-level OR window, x/y are root-relative
          int32_t grabLocalX = root_x - gv.x;
          int32_t grabLocalY = root_y - gv.y;
          target = pick_motion_target(*ctx, activeGrab.grabWindow,
                                     grabLocalX, grabLocalY);
        }
      }
      if (!target) {
        // Still nothing → fall back to grab window if it wants motion
        if (activeGrab.eventMask & (1u << 6)) // PointerMotionMask
          target = activeGrab.grabWindow;
      }
    } else {
      // owner_events=False: route directly to grab window
      if (activeGrab.eventMask & (1u << 6)) // PointerMotionMask
        target = activeGrab.grabWindow;
      else
        target = 0;
    }
  } else if (ctx->input().drag_xid && !hostCorrected) {
    // No active grab, but button drag in progress AND pointer is still
    // over the same host (e.g., scrollbar drag within xterm).
    // When hostCorrected is true, the pointer moved to a different
    // top-level window (popup menu), so ignore drag_xid and route
    // to the actual window under the pointer.
    target = ctx->input().drag_xid;
  } else {
    // Normal case: route based on WINDOW-LOCAL coords
    target = pick_motion_target(*ctx, host_xid, win_x, win_y);
  }

#ifndef NDEBUG
  // Trace drag-routed or grab-routed motion (high-frequency, but only during grab/drag)
  if (ctx->input().drag_xid || haveGrab) {
    static int dragMotionCount = 0;
    if (++dragMotionCount <= 20) { // first 20 only, to avoid flood
      fprintf(stderr,
              "[DRAG_MOTION] host=0x%08X target=0x%08X drag=0x%08X grab=%s grabWin=0x%08X win=(%d,%d)\n",
              (unsigned)host_xid, (unsigned)target, (unsigned)ctx->input().drag_xid,
              haveGrab ? "YES" : "no",
              haveGrab ? (unsigned)activeGrab.grabWindow : 0u,
              (int)win_x, (int)win_y);
    }
    if (dragMotionCount == 20) {
      fprintf(stderr, "[DRAG_MOTION] (further traces suppressed)\n");
    }
  }
#endif

  if (!target) return;

  // The cursor is applied to the *host* (Cocoa window), but chosen from the *effective target*.
  uint32_t host = host_xid;
  if (!host) host = ctx->input().focus_host;
  if (!host) host = ctx->windows().topLevelAncestorOf(target); // last-resort

  const uint32_t cursorTarget = ctx->input().routePointer(target); // respects drag_xid/pointer_xid/focus_xid
  maybeApplyCursor(*ctx, host, cursorTarget);
  
  // Send MotionNotify with ROOT coords (root_x/root_y)
  ev->sendMotionNotify(*ctx, target, root_x, root_y, buttons, mods);
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

    // Walk up until you find a motion listener.
    uint32_t cur = best;
    while (cur) {
      WindowView vw{};
      if (!ctx.windows().snapshot(cur, vw)) break;
      if (vw.event_mask & motionMask) return cur;
      cur = vw.parent_xid;
    }

    return 0;
  }
  
}
