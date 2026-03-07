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

  // ---- Check for active pointer grab (GrabPointer) ----
  // Popup menus (xterm Ctrl+click) grab the pointer after mapping.
  // While a grab is active, ALL motion and crossing events must route
  // to the grab window (or its subwindows if ownerEvents is true).
  x11::PointerGrab activeGrab{};
  const bool haveGrab = ctx->grabs().getPointerGrab(activeGrab) && activeGrab.active;

  // ---- Child-to-child crossing events ----
  // When not dragging, pick the deepest mapped child under the pointer
  // and generate EnterNotify/LeaveNotify if the pointer moved to a
  // different window.  Xaw Command widgets rely on these for hover
  // highlighting (highlight on enter, unhighlight on leave).
  // Without this, crossing events only fire on NSWindow enter/leave,
  // leaving button highlighting stuck on the first button entered.
  if (ctx->input().drag_xid == 0) {
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

  // During an active grab/drag, route motion to the grab owner.
  // This is essential for scrollbar drag: xterm's scrollbar installs a
  // passive grab via GrabButton; when button 1 is pressed, drag_xid is
  // set to the scrollbar child. All subsequent motion must go there.
  uint32_t target;
  if (ctx->input().drag_xid) {
    target = ctx->input().drag_xid;
  } else if (haveGrab) {
    // Active pointer grab (GrabPointer): route motion to grab window
    // if the grab's event mask includes PointerMotionMask (bit 6).
    // This is critical for popup menus where the button is already
    // released (drag_xid == 0) but the grab is still active.
    if (activeGrab.eventMask & (1u << 6)) { // PointerMotionMask
      target = activeGrab.grabWindow;
    } else {
      target = 0;  // grab active but doesn't want motion events
    }
  } else {
    // Normal case: route based on WINDOW-LOCAL coords
    target = pick_motion_target(*ctx, host_xid, win_x, win_y);
  }

#ifndef NDEBUG
  // Trace drag-routed or grab-routed motion (high-frequency, but only during grab/drag)
  if (ctx->input().drag_xid || haveGrab) {
    static int dragMotionCount = 0;
    if (++dragMotionCount <= 5) { // first 5 only, to avoid flood
      fprintf(stderr,
              "[DRAG_MOTION] host=0x%08X target=0x%08X drag=0x%08X grab=%s grabWin=0x%08X win=(%d,%d)\n",
              (unsigned)host_xid, (unsigned)target, (unsigned)ctx->input().drag_xid,
              haveGrab ? "YES" : "no",
              haveGrab ? (unsigned)activeGrab.grabWindow : 0u,
              (int)win_x, (int)win_y);
    }
    if (dragMotionCount == 5) {
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
    
    // Walk up until you find a motion listener.
    uint32_t cur = best;
    while (cur) {
      WindowView vw{};
      if (!ctx.windows().snapshot(cur, vw)) break;
      if (vw.event_mask & (1u << 6)) return cur; // PointerMotionMask
      cur = vw.parent_xid;
    }
    
    return 0;
  }
  
}
