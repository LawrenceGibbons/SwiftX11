//
//  XProtoServerBridge.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/21/26.
//

#include <atomic>
#include <mutex>
#include <cstddef>
#include <cstdio>
#include <array>
#include <cstring>
#include <deque>
#include <cstdint>
#include <vector>

#include "XProtoServerBridge.h"
#include "Core/XProtoServer.hpp"        // owns ctx_, eventOps_, transport_
#include "Core/XI2EventMask.hpp"
#include "Core/XConstants.hpp"
#include "Ops/EventOps.hpp"
#include "Utils/GrabRoute.hpp"        // Phase B2: grab-time routing (xorg DeliverGrabbedEvent)
#include "Utils/GrabChoreography.hpp" // implicit-grab activation / release crossings
#include "Utils/EnterLeave.hpp"       // Phase D: DoEnterLeaveEvents choreography
#include "Utils/FocusEvents.hpp"      // Phase D: DoFocusEvents choreography

namespace {
// xorg CheckDeviceGrabs (dix/events.c:4185-4207): passive grabs are checked
// along the sprite trace from the ROOT down to the sprite window, so an
// ancestor's GrabButton wins over a descendant's — Phase G, L9.  (The old
// walk went child-up.)  `under` is the sprite window, `host` its toplevel.
bool checkPassiveGrabsRootDown(x11::XProtoContext& ctx, uint32_t under, uint32_t host,
                               uint8_t button, uint16_t x11Mods, x11::PassiveGrab& out) {
  std::vector<uint32_t> trace;   // sprite window → toplevel
  for (uint32_t w = under; w && trace.size() < 64;) {
    trace.push_back(w);
    if (w == host) break;
    x11::WindowView vw{};
    if (!ctx.windows().snapshot(w, vw)) break;
    w = vw.parent_xid;
  }
  if (ctx.grabs().match(x11::kRootXid, button, x11Mods, out)) return true;   // root first
  for (size_t i = trace.size(); i-- > 0;)
    if (ctx.grabs().match(trace[i], button, x11Mods, out)) return true;
  return false;
}

// xorg CheckDeviceGrabs for a KEYBOARD event (dix/events.c:4183-4206): passive
// GrabKey grabs are checked along the FOCUS trace from the root down to the
// focus window, then (when the pointer lies inside the focus subtree) down the
// sprite trace below the focus window.  focus == PointerRoot (our kRootXid)
// checks the whole sprite trace; focus == None (0) checks nothing further.
// C2.  `key` is the X11 keycode, `x11Mods` the modifier state before the key.
bool checkPassiveKeyGrabsRootDown(x11::XProtoContext& ctx, uint32_t focus, uint32_t sprite,
                                  uint8_t key, uint16_t x11Mods, x11::PassiveKeyGrab& out) {
  // Root grabs first (root is trace[0]); snapshot() fails on the root XID so it
  // is never reached by the parent walks below.
  if (ctx.grabs().matchKey(x11::kRootXid, key, x11Mods, out)) return true;

  // Check a leaf→toplevel chain root-down (excludes the root XID).
  auto walkUpChecking = [&](uint32_t leaf, uint32_t stopExclusive) -> bool {
    std::vector<uint32_t> tr;
    for (uint32_t w = leaf; w && w != x11::kRootXid && w != stopExclusive && tr.size() < 64;) {
      tr.push_back(w);
      x11::WindowView vw{};
      if (!ctx.windows().snapshot(w, vw)) break;
      w = vw.parent_xid;
    }
    for (size_t i = tr.size(); i-- > 0;)
      if (ctx.grabs().matchKey(tr[i], key, x11Mods, out)) return true;
    return false;
  };

  if (focus == x11::kRootXid) {                 // PointerRoot → sprite trace
    return sprite ? walkUpChecking(sprite, 0) : false;
  }
  if (focus == 0) return false;                 // None → nothing further

  // Real focus window: root → focus, then, if the pointer is inside the focus
  // subtree, the sprite trace below focus (focus exclusive).
  if (walkUpChecking(focus, 0)) return true;
  if (sprite && sprite != focus && x11::wintree::isAncestor(ctx, focus, sprite))
    return walkUpChecking(sprite, /*stopExclusive=*/focus);
  return false;
}

// xorg EventIsDeliverable: a window takes a button event if it selected it
// at either level (core mask or its XI2 selection).
bool wantsButton(x11::XProtoContext& ctx, uint32_t xid, bool isDown) {
  if (!xid) return false;
  const x11::WindowView* vw = ctx.window(xid);
  if (!vw || vw->owner_fd <= 0) return false;
  const bool coreWant = isDown ? (vw->event_mask & x11::mask::ButtonPress)   != 0
                               : (vw->event_mask & x11::mask::ButtonRelease) != 0;
  const uint32_t xi2bit = isDown ? x11::xi2::kButtonPressMask : x11::xi2::kButtonReleaseMask;
  return coreWant || (vw->xi2_mask & xi2bit) != 0;
}

// Normal-delivery target for a button event at `under` (xorg
// DeliverDeviceEvents): climb by selection up to the toplevel, fenced by
// do_not_propagate (§2.8).  0 = nobody wants it.
uint32_t buttonDeliveryWindow(x11::XProtoContext& ctx, uint32_t under, uint32_t host, bool isDown) {
  if (wantsButton(ctx, under, isDown)) return under;
  const uint32_t dnpBit = isDown ? x11::mask::ButtonPress : x11::mask::ButtonRelease;
  uint32_t cur = under;
  for (int safety = 0; cur && cur != host && safety < 64; safety++) {
    x11::WindowView vw{};
    if (!ctx.windows().snapshot(cur, vw)) return 0;
    if (vw.do_not_propagate_mask & dnpBit) return 0;
    cur = vw.parent_xid;
    if (wantsButton(ctx, cur, isDown)) return cur;
  }
  return 0;
}
} // namespace
#include "Ops/QueryOps.hpp"   // (and later AtomOps.hpp, WindowOps.hpp, etc.)
#include "Core/WindowTable.hpp"
#include "Core/XProtoModules.hpp"
#include "Core/XProtoContext.hpp"
//#include "x11_window_set_mapped.h"
#include "Core/GCTable.hpp"
#include "Core/HostResize.hpp"
#include "Transport/XProtoDaemon.hpp"
#include "Core/WindowView.hpp"
#include "Ops/EventOps.hpp"
#include "Ops/SelectionOps.hpp"
#include "XProtoNotifyBridge.hpp"
#include "Core/XEventMask.hpp"
#include "Utils/BackgroundFill.hpp"
#include "Core/X11Modifiers.hpp"
#include "Core/InputRouting.hpp"
#include "Core/GrabTable.hpp"
#include "Core/timestamp.hpp"
#include "SwiftX11Bridge.h"
#include "WireEvents.hpp"
#include "Core/CursorRouting.hpp"
#include "Core/DrawableRW.hpp"
#include "Damage.hpp"
#include "Core/HostCommandQueue.hpp"
#include "Utils/DragTrace.hpp"
#include "Core/XClient.hpp"
#include "Core/ClipboardAtoms.hpp"
#include "Core/XConstants.hpp"
#include "Utils/WireLE.hpp"
#include "Utils/TraceDefs.hpp"

using x11::HostCmdType;
using x11::HostCmd;

// g_daemon is the process-lifetime daemon instance (defined later in this file).
// Access the server via g_daemon.server().

// Forward reference to daemon (defined at bottom of file, used by bridge functions)
namespace { x11::XProtoDaemon* g_daemon_ptr = nullptr; }


extern "C" void x11_cpp_notify_init(void* ctx_ptr, void* event_ops_ptr, void* queue_ptr);
extern "C" void x11_cpp_notify_shutdown(void);

extern "C" x11::XProtoServer* x11_proto_bridge_get_server(void)
{
  return g_daemon_ptr ? g_daemon_ptr->server() : nullptr;
}

extern "C" x11::XProtoDaemon* x11_proto_bridge_get_daemon(void)
{
  return g_daemon_ptr;
}

extern "C" void x11_proto_bridge_begin_session(int client_fd,
                                               uint32_t rid_base,
                                               uint32_t rid_mask)
{
  if (client_fd < 0) return;
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;

  // Create per-session client and wire into server context.
  auto* client = new x11::XClient(srv->ctx(), srv->eventOps(),
                                  client_fd, rid_base, rid_mask);
  srv->ctx().setClient(client);

  // Initialize the notify bridge pointers.
  void* ctx_ptr = (void*)&srv->ctx();
  void* ev_ptr  = (void*)&srv->eventOps();
  void* q_ptr   = (void*)&srv->ctx().transport().notifyQueue();
  x11_cpp_notify_init(ctx_ptr, ev_ptr, q_ptr);
}


extern "C" void x11_proto_bridge_end_session(int client_fd)
{
  x11_cpp_notify_shutdown();

  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;

  // Erase windows owned by this client fd (child-first order).
  std::vector<uint32_t> owned = srv->ctx().windows().eraseOwnedBy(client_fd);
  for (uint32_t wid : owned) {
    x11_ui_push_destroy(wid);
  }

  // Reset per-session server state so the next client starts clean.
  srv->ctx().grabs().clearAll();
  srv->ctx().input() = x11::InputState{};

  // Destroy the per-session client.
  x11::XClient* client = srv->ctx().client();
  srv->ctx().clearClient();
  delete client;
}


extern "C" void x11_proto_bridge_note_last_seq(uint16_t seq)
{
  auto* srv = x11_proto_bridge_get_server();
  if (srv && srv->ctx().hasClient())
    srv->ctx().transport().noteLastSeq(seq);
}


// Forward declaration (defined later in this file).
extern "C" int x11_cpp_get_abs_pos_in_host(uint32_t host, uint32_t xid,
                                           int32_t* out_abs_x,
                                           int32_t* out_abs_y);

// ---- Blit legacy C framebuffers into the Swift host surface ----
// X11 spec: server paints the window's background before delivering Expose.
// Called when the surface is ready (after SetPresentable).  Earlier attempts
// at MapWindow time may have failed because the Swift surface wasn't yet
// registered.
static void fillWindowBackgroundIfReady(x11::XProtoContext& ctx, uint32_t wid) {
  // Check for background pixmap first (takes priority over solid pixel)
  uint32_t bgPixmap = 0;
  if (ctx.windows().resolveBackgroundPixmapForClear(wid, bgPixmap)) {
    x11::DrawableRW dst{};
    if (!x11::resolveDrawableRW(ctx, wid, dst)) return;
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) return;

#if X11_TRACE_PRESENT_ENABLED
    TS_FPRINTF("[BG_FILL_RETRY] wid=0x%08X pixmap=0x%08X wh=%ux%u stride=%u\n",
            (unsigned)wid, (unsigned)bgPixmap,
            (unsigned)dst.w, (unsigned)dst.h, (unsigned)dst.stridePixels);
#endif

    x11::tilePixmapFill(ctx, bgPixmap, dst, 0, 0, (int)dst.w, (int)dst.h);
    return;
  }

  // Fall back to solid-color background
  uint32_t bg = 0;
  if (!ctx.windows().resolveBackgroundForClear(wid, bg)) {
    return;
  }

  x11::DrawableRW dst{};
  if (!x11::resolveDrawableRW(ctx, wid, dst)) {
#if X11_TRACE_PRESENT_ENABLED
    TS_FPRINTF("[BG_FILL_RETRY] wid=0x%08X SKIP resolve failed\n", (unsigned)wid);
#endif
    return;
  }
  if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) return;

#if X11_TRACE_PRESENT_ENABLED
  TS_FPRINTF("[BG_FILL_RETRY] wid=0x%08X bg=0x%08X wh=%ux%u stride=%u off=(%d,%d)\n",
          (unsigned)wid, (unsigned)bg,
          (unsigned)dst.w, (unsigned)dst.h, (unsigned)dst.stridePixels,
          (int)dst.offsetX, (int)dst.offsetY);
#endif

  if (dst.numOccluded > 0) {
    for (uint16_t y = 0; y < dst.h; y++) {
      uint32_t* row = dst.pixels32 + (size_t)y * (size_t)dst.stridePixels;
      for (uint16_t x = 0; x < dst.w; x++) {
        if (!dst.isOccluded((int32_t)x, (int32_t)y)) row[x] = bg;
      }
    }
  } else {
    for (uint16_t y = 0; y < dst.h; y++) {
      uint32_t* row = dst.pixels32 + (size_t)y * (size_t)dst.stridePixels;
      for (uint16_t x = 0; x < dst.w; x++) {
        row[x] = bg;
      }
    }
  }
}

// Draw the server-side border around a child window (re-expose path).
// Same logic as fillWindowBorder in WindowOps.cpp but for the SetPresentable/
// SurfaceResized re-expose path, where surfaces are guaranteed ready.
static void fillWindowBorderIfReady(x11::XProtoContext& ctx, uint32_t childXid) {
  x11::WindowView cv{};
  if (!ctx.windows().snapshot(childXid, cv)) return;
  if (cv.border_width == 0) return;
  if (cv.parent_xid == 0 || cv.parent_xid == 1) return;

  x11::DrawableRW parentDst{};
  if (!x11::resolveDrawableRW(ctx, cv.parent_xid, parentDst)) return;
  if (!parentDst.pixels32 || parentDst.w == 0 || parentDst.h == 0) return;

  const int32_t bw = (int32_t)cv.border_width;
  const uint32_t bp = cv.border_pixel;
  const int32_t bx = (int32_t)cv.x;
  const int32_t by = (int32_t)cv.y;
  const int32_t totalW = (int32_t)cv.w + 2 * bw;

  auto fillRect = [&](int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > (int32_t)parentDst.w) rw = (int32_t)parentDst.w - rx;
    if (ry + rh > (int32_t)parentDst.h) rh = (int32_t)parentDst.h - ry;
    if (rw <= 0 || rh <= 0) return;
    for (int32_t y = ry; y < ry + rh; y++) {
      uint32_t* row = parentDst.pixels32 + (size_t)y * parentDst.stridePixels;
      for (int32_t x = rx; x < rx + rw; x++) {
        row[x] = bp;
      }
    }
  };

  fillRect(bx, by, totalW, bw);                              // top
  fillRect(bx, by + bw + (int32_t)cv.h, totalW, bw);         // bottom
  fillRect(bx, by + bw, bw, (int32_t)cv.h);                   // left
  fillRect(bx + bw + (int32_t)cv.w, by + bw, bw, (int32_t)cv.h); // right

#if X11_TRACE_PRESENT_ENABLED
  TS_FPRINTF("[BORDER_RETRY] childXid=0x%08X parent=0x%08X bw=%d bp=0x%08X at=(%d,%d) total=%dx%d\n",
          (unsigned)childXid, (unsigned)cv.parent_xid,
          (int)bw, (unsigned)bp, (int)bx, (int)by, (int)totalW, (int)totalH);
#endif
}

// Send a full-window Expose to a single window.
// `count` indicates how many more Expose events will follow for this window's
// client, allowing the client to defer redrawing until count==0.
static inline void sendExposeNow(x11::XProtoContext& ctx,
                                 x11::EventOps& /*evOps*/,
                                 uint32_t wid,
                                 uint16_t count = 0)
{
  const x11::WindowView* wv = ctx.window(wid);
  if (!wv) {
#if X11_TRACE_LIFECYCLE_ENABLED
    TS_FPRINTF("[EXPOSE_SEND] wid=0x%08X SKIP (no WindowView)\n", (unsigned)wid);
#endif
    return;
  }

#if X11_TRACE_LIFECYCLE_ENABLED
  TS_FPRINTF("[EXPOSE_SEND] wid=0x%08X wh=%ux%u mapped=%d evmask=0x%08X count=%u\n",
          (unsigned)wid, (unsigned)wv->w, (unsigned)wv->h,
          (int)wv->mapped, (unsigned)wv->event_mask, (unsigned)count);
#endif

  auto ev = x11::wireev::buildExpose(ctx.transport().lastSeq(),
                                   wid,
                                   0, 0,
                                   wv->w, wv->h,
                                   count);
  bool sent = ctx.transport().sendEvent32(wid, ev.data());
  (void)sent;
}

// Re-expose a host window AND all its mapped descendants.
// Needed because xeyes (and many X11 clients) draw to a child window,
// not the host.  If we only re-expose the host, the child never redraws.
//
// X11 spec requires the server to paint backgrounds before delivering Expose.
// The initial fillWindowBackground at MapWindow time may have failed because
// the Swift surface wasn't registered yet.  Now (post-SetPresentable) the
// surface is ready, so we retry the fill for each window before its Expose.
static inline void sendExposeSubtree(x11::XProtoContext& ctx,
                                     x11::EventOps& evOps,
                                     uint32_t hostXid)
{
#if X11_TRACE_LIFECYCLE_ENABLED
  TS_FPRINTF("[EXPOSE_SUBTREE] host=0x%08X\n", (unsigned)hostXid);
#endif

  // Fill background + Expose the host itself.
  fillWindowBackgroundIfReady(ctx, hostXid);

  // Collect mapped descendants and fill their borders + backgrounds first
  // (X11 spec: server paints backgrounds before delivering Expose).
  auto kids = ctx.windows().descendantsOf(hostXid);

#if X11_TRACE_LIFECYCLE_ENABLED
  TS_FPRINTF("[EXPOSE_SUBTREE] host=0x%08X descendants=%zu\n",
          (unsigned)hostXid, kids.size());
#endif

  // Pre-fill all backgrounds/borders, then collect mapped kids for Expose.
  std::vector<uint32_t> mappedKids;
  mappedKids.reserve(kids.size());
  for (uint32_t kid : kids) {
    x11::WindowView kv{};
    if (!ctx.windows().snapshot(kid, kv)) continue;
    if (!kv.mapped) continue;

    fillWindowBorderIfReady(ctx, kid);
    fillWindowBackgroundIfReady(ctx, kid);

#if X11_TRACE_LIFECYCLE_ENABLED
    {
      x11::DrawableRW dbgDst{};
      bool resolved = x11::resolveDrawableRW(ctx, kid, dbgDst);
      TS_FPRINTF("[EXPOSE_SUBTREE] kid=0x%08X resolved=%d wh=%ux%u off=(%d,%d) stride=%u\n",
              (unsigned)kid, (int)resolved,
              resolved ? (unsigned)dbgDst.w : 0u,
              resolved ? (unsigned)dbgDst.h : 0u,
              resolved ? (int)dbgDst.offsetX : 0,
              resolved ? (int)dbgDst.offsetY : 0,
              resolved ? (unsigned)dbgDst.stridePixels : 0u);
    }
#endif
    mappedKids.push_back(kid);
  }

  // Send Expose events.  count=0 because we send exactly one Expose per
  // window (full-window rect).  X11 spec: count is per-WINDOW ("number of
  // Expose events to follow for this window"), NOT per-client-batch.
  sendExposeNow(ctx, evOps, hostXid);
  for (uint32_t kid : mappedKids) {
    sendExposeNow(ctx, evOps, kid);
  }
}


// Process a single host command.  Called from the daemon's drainHostCommands()
// (one command at a time with the correct client activated) and from the
// legacy flush_notify_queue path.
static void processOneHostCmd(x11::XProtoServer* srv,
                              x11::XProtoContext& ctx,
                              const x11::HostCmd& c)
{
  using x11::HostCmdType;
  switch (c.type) {
        // ------------------- RootlessResize
        case HostCmdType::RootlessResize:
          // Runs on xproto thread now: safe vs drawing + fb resize.
          // applyRootlessResize updates host geometry and sends
          // ConfigureNotify to host (so xterm reconfigures children).
          // Children get BG fill + Expose when xterm sends ConfigureWindow
          // for them (handled in WindowAttrOps::handleConfigureWindow).
          applyRootlessResize(ctx, c.xid, c.w_px, c.h_px);
          break;

        // ------------------- SetPresentable
        case HostCmdType::SetPresentable: {
          // NOTE: Do NOT guard with "if already presentable, skip".
          // updateSurface() (called from x11_surface_update on the Swift main
          // thread) sets setPresentable(true) when registering the surface.
          // That happens BEFORE this host command is processed on the server
          // thread.  A guard checking presentable would ALWAYS skip, and
          // sendExposeSubtree would never run — no Expose events would be
          // sent to popup menu children, causing blank popup text.
          //
          // Duplicate SetPresentable is prevented on the Swift side: X11View
          // has a single didNotifyPresentable flag, and X11Renderer delegates
          // to owner?.notifyPresentableOnce() instead of posting independently.

#if X11_TRACE_LIFECYCLE_ENABLED
          TS_FPRINTF("[SET_PRESENTABLE] xid=0x%08X\n", (unsigned)c.xid);
          {
            x11::SurfaceDesc dbgS{};
            bool hasSurf = ctx.surfaces().get(c.xid, dbgS);
            TS_FPRINTF("[SET_PRESENTABLE] xid=0x%08X hasSurface=%d surfWH=%ux%u bpr=%u ptr=%p\n",
                    (unsigned)c.xid, (int)hasSurf,
                    (unsigned)dbgS.w, (unsigned)dbgS.h,
                    (unsigned)dbgS.bytesPerRow, dbgS.ptr);
          }
#endif
          ctx.windows().setPresentable(c.xid, true);

          // Write full-window damage to the shared accumulator and signal
          // so any content drawn before the surface was presentable gets shown.
          {
            x11::WindowView pv{};
            if (ctx.windows().snapshot(c.xid, pv)) {
              x11_shared_damage_union(c.xid, 0, 0, (int32_t)pv.w, (int32_t)pv.h);
              x11_ui_push_damage(c.xid, 0, 0, (int32_t)pv.w, (int32_t)pv.h);
            }
          }
          ctx.windows().markDirty(c.xid);

          // If flushPendingMaps rescued this window's size (shrunk-below-peak),
          // the client still thinks it's at the pre-rescue dimensions and will
          // draw its layout at the smaller size.  Send ConfigureNotify now (we
          // have a valid client transport here) so the client relayouts at
          // the real size BEFORE we send Expose and it paints.
          if (srv->takeNeedsPostMapConfigureNotify(c.xid)) {
            x11::WindowView pv{};
            if (ctx.windows().snapshot(c.xid, pv)) {
              auto ev = x11::wireev::buildConfigureNotify(
                ctx.transport().lastSeq(),
                /*event*/ c.xid, /*window*/ c.xid,
                /*aboveSibling*/ 0,
                pv.x, pv.y, pv.w, pv.h,
                pv.border_width, pv.override_redirect);
              // ICCCM: WM-synthesized ConfigureNotify must carry the
              // send_event bit — Swing branches on it for inset/coordinate
              // interpretation of the rescue geometry.
              ev[0] |= 0x80;
              (void)ctx.transport().sendEvent32(c.xid, ev.data());
#ifndef NDEBUG
              TS_FPRINTF("[GEOM] wid=0x%08X source=POST_MAP_CONFIGNOTIFY size=%ux%u@(%d,%d)\n",
                      (unsigned)c.xid, (unsigned)pv.w, (unsigned)pv.h,
                      (int)pv.x, (int)pv.y);
#endif
            }
          }

          // Re-expose the host and all mapped descendants so clients
          // redraw into the Swift surface now that it's presentable.
          sendExposeSubtree(ctx, srv->eventOps(), c.xid);
          break;
        }

        // ------------------- SurfaceResized
        // Surface dimensions changed.  Two distinct cases:
        //
        // A) Initial presentation (not yet presentable): Swift's setContentSize
        //    completed after the initial surface registration.  Child windows at
        //    far offsets (e.g., scrollbar) were clipped to zero by the first
        //    sendExposeSubtree.  Full re-expose is needed.
        //
        // B) Live resize (already presentable): Surface reallocated during a
        //    Cocoa drag.  Do NOT call sendExposeSubtree here — it fills the
        //    entire host background which destructively wipes child content
        //    (scrollbar etc.) that was drawn by the client.  The RootlessResize
        //    handler (which follows shortly) sends ConfigureNotify so the client
        //    repositions children via ConfigureWindow, and that handler now fills
        //    each child's background at its new position.
        case HostCmdType::SurfaceResized: {
          x11::WindowView sv{};
          bool haveSV = ctx.windows().snapshot(c.xid, sv);

          // Gate on the live-resize flag (carried in w_px), NOT the old
          // one-shot surface_resize_exposed: that flag meant only the
          // FIRST size change ever re-exposed — a client-driven grow
          // later in the window's life (dialog resized via
          // ConfigureWindow) reallocated the surface after Expose had
          // already been sent, silently dropping the client's paint
          // (white/clipped dialog; review 2026-08-31 §3.2).
          const bool inLiveResize = (c.w_px != 0);
          if (haveSV && !inLiveResize) {
            // Non-live surface change — full re-expose needed.
#if X11_TRACE_RESIZE_ENABLED
            TS_FPRINTF("[SURFACE_RESIZED] xid=0x%08X (non-live) -> re-expose subtree\n",
                    (unsigned)c.xid);
#endif
            sendExposeSubtree(ctx, srv->eventOps(), c.xid);

            // Write full-window damage for the re-expose.
            x11_shared_damage_union(c.xid, 0, 0, (int32_t)sv.w, (int32_t)sv.h);
            x11_ui_push_damage(c.xid, 0, 0, (int32_t)sv.w, (int32_t)sv.h);
            ctx.windows().markDirty(c.xid);
            ctx.windows().setSurfaceResizeExposed(c.xid, true); // vestigial
          } else {
            // Case B: live resize — skip EVERYTHING.
            // Do NOT call sendExposeSubtree (destructive BG fill wipes children).
            // Do NOT report damage here — it triggers a premature present that
            // shows the surface with old/copied content before the client has
            // redrawn at the new size.  The RootlessResize handler (which
            // follows ~16ms later) updates geometry + reports damage, and the
            // ConfigureWindow handler (triggered by xterm's response to
            // ConfigureNotify) fills child backgrounds + sends Expose.
#if X11_TRACE_RESIZE_ENABLED
            TS_FPRINTF("[SURFACE_RESIZED] xid=0x%08X (resize) -> skip\n",
                    (unsigned)c.xid);
#endif
          }
          break;
        }

        // ------------------- ExposeChildren
        // Sent at end of live resize to re-expose children whose content may
        // have been lost during surface reallocation.  Fills borders and
        // backgrounds (matching sendExposeSubtree behaviour) because the
        // surface was memset to white during resize — server-drawn borders
        // and backgrounds must be repainted before sending Expose events.
        case HostCmdType::ExposeChildren: {
          // Fill + Expose the host and all mapped descendants.
          fillWindowBackgroundIfReady(ctx, c.xid);

          // Collect mapped kids, fill borders/backgrounds, then send Expose.
          // count=0: one Expose per window (count is per-window, not per-batch).
          auto kids = ctx.windows().descendantsOf(c.xid);
          sendExposeNow(ctx, srv->eventOps(), c.xid);
          for (uint32_t kid : kids) {
            x11::WindowView kv{};
            if (!ctx.windows().snapshot(kid, kv)) continue;
            if (!kv.mapped) continue;
            fillWindowBorderIfReady(ctx, kid);
            fillWindowBackgroundIfReady(ctx, kid);
            sendExposeNow(ctx, srv->eventOps(), kid);
          }
          {
            x11::WindowView sv{};
            if (ctx.windows().snapshot(c.xid, sv)) {
              damageOrDirty(ctx, c.xid, 0, 0, (int32_t)sv.w, (int32_t)sv.h);
            }
          }
          break;
        }

        // ------------------- PointerMove
        case HostCmdType::PointerMove: {
          x11::notify::postMotion(c.xid,
                                  c.win_x_u, c.win_y_u,
                                  c.root_x_u, c.root_y_u,
                                  c.deliver,
                                  c.buttonsMask, c.modsMask);
          break;
        }


        // ------------------- PointerEnter
        case HostCmdType::PointerEnter: {
          const uint32_t host = c.xid ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // M16: use THIS command's host-local coordinates.  The previous code
          // read the last motion's, which belong to the window just LEFT, so
          // the Enter could target the wrong child of the new host and carry
          // the old host's event coordinates.  Root coords travel with the
          // command too (v1.20.0.26): deriving them from the cached host
          // origin went wrong whenever an outside-the-view move had fed that
          // cache clamped local coordinates (Enter at root (2301,1020) for a
          // 300-px-tall window at y=600 — origin 721 + clamped local 299).
          const int32_t rx = c.root_x_u, ry = c.root_y_u;
          ctx.input().updateMotion(host, c.win_x_u, c.win_y_u, rx, ry,
                                   ctx.input().buttons, c.modsMask);

          uint32_t under = x11::pickDeepestMappedWindowAtHostPoint(ctx, host,
                                                                  c.win_x_u, c.win_y_u);
          if (!under) under = host;

          // The window the pointer is leaving: xorg's sprite window.  0 =
          // root (over no X window); a window in ANOTHER host when AppKit
          // ordered mouseEntered(B) before mouseExited(A) (M16) — then the
          // Nonlinear Leave(A)/Enter(B) pair goes out here and A's late
          // PointerLeave finds nothing to leave.
          const uint32_t prev = ctx.input().drag_xid ? 0 : ctx.input().pointer_xid;

          // Pointer ownership should be the window under the pointer, not the host.
          ctx.input().enter(under);

          // Apply cursor to the Cocoa host window, choosing cursor from the routed pointer target.
          const uint32_t cursorTarget = ctx.input().routePointer(under);
          maybeApplyCursor(ctx, host, cursorTarget);

          // Phase D (M15/M16): xorg DoEnterLeaveEvents(prev, under) — both
          // levels, relation-derived detail, Virtual events on the windows
          // between, the grab filter per delivery.
          x11::enterleave::doEnterLeave(ctx, srv->eventOps(), prev, under, x11::notifymode::kNormal,
                                        rx, ry, ctx.input().buttons, c.modsMask);
          break;
        }


        // ------------------- PointerLeave
        case HostCmdType::PointerLeave: {
          const uint32_t host = c.xid ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // Window that currently "has" the pointer (drag wins) — but only if
          // it lives in THIS host (M16): AppKit does not order mouseExited(A)
          // before mouseEntered(B) for overlapping windows, and when the Enter
          // for B landed first pointer_xid already points into B, so the Leave
          // went to B's window and A never got one.
          uint32_t leaveWin = 0;
          const uint32_t dx = ctx.input().drag_xid;
          const uint32_t px = ctx.input().pointer_xid;
          if (dx && ctx.windows().topLevelAncestorOf(dx) == host)      leaveWin = dx;
          else if (px && ctx.windows().topLevelAncestorOf(px) == host) leaveWin = px;
          // px == 0: the sprite is already on root — AppKit sends a second
          // mouseExited when the app deactivates after the pointer has left,
          // and the old `host` fallback turned it into a spurious Leave
          // (v1.20.0.26).  px in another host: the Enter there already
          // emitted the Leave for this one (Phase D).

          // After leaving, cursor should usually fall back (focus/host/inherit).
          if (leaveWin) ctx.input().leave(leaveWin);
          const uint32_t cursorTarget = ctx.input().routePointer(host);
          maybeApplyCursor(ctx, host, cursorTarget);

          // Phase D: xorg DoEnterLeaveEvents(leaveWin, root), at the exit
          // event's own root position.
          if (leaveWin) {
            x11::enterleave::doEnterLeave(ctx, srv->eventOps(), leaveWin, /*to=root*/0,
                                          x11::notifymode::kNormal,
                                          c.root_x_u, c.root_y_u,
                                          ctx.input().buttons, c.modsMask);
          }
          break;
        }


        // ------------------- Focus
        // Emulates WM SetInputFocus: FocusIn always goes to the HOST
        // (top-level shell) so the toolkit (Xt) can internally propagate
        // focus to the correct child widget.  Uses sendFocusEventDirect
        // to bypass FocusChangeMask (matching real SetInputFocus behaviour).
        case HostCmdType::Focus: {
          const uint32_t host = c.xid;
          if (!host) break;

          const uint32_t oldFocus = ctx.input().focus_xid;

          if (c.focused) {
            // Skip if this host already has focus — prevents duplicate
            // processing from the two Cocoa notification paths
            // (X11WindowController + WindowRegistry) that both fire
            // didBecomeKey.
            if (ctx.input().focus_host == host) {
              break;
            }

            ctx.input().focus_host = host;

            // Focus the HOST — let the toolkit propagate to children
            // via SetInputFocus (opcode 42).
            ctx.input().focus_xid = host;
            // pointer_xid is NOT touched (v1.20.0.28): gaining key status
            // used to claim the pointer for the host silently, so the real
            // entry that followed found the sprite already there and emitted
            // no Enter, while a later exit still emitted a Leave.  Crossings
            // come only from pointer moves (PointerEnter/Leave, motion).

            // ICCCM WM_TAKE_FOCUS: if client advertises it in WM_PROTOCOLS,
            // send ClientMessage so the client calls SetInputFocus itself.
            //
            // Bounce detection: Java/Swing responds to WM_TAKE_FOCUS by
            // calling SetInputFocus (which may target a window on a different
            // host) and sometimes raising the target host window.  This causes
            // Cocoa to fire didBecomeKey for the other host, which triggers
            // another WM_TAKE_FOCUS — creating an A→B→A bounce loop between
            // dialog and main window.
            //
            // We track the last two WM_TAKE_FOCUS targets (prev, last).
            // If prev == current host, we're in a bounce and suppress.
            // Same-host duplicate (last == host) is also suppressed.
            // Tracking is cleared on button press (genuine user interaction).
            x11::WindowView hostView{};
            if (ctx.windows().snapshot(host, hostView) && hostView.wants_take_focus) {
              const bool sameHost  = (ctx.input().take_focus_last_ == host);
              const bool abaBounce = (ctx.input().take_focus_prev_ == host);
              if (!sameHost && !abaBounce) {
                // Shift history and send
                ctx.input().take_focus_prev_ = ctx.input().take_focus_last_;
                ctx.input().take_focus_last_ = host;
                uint8_t ev[32] = {0};
                ev[0] = 33;       // ClientMessage
                ev[1] = 32;       // format = 32
                x11::wire::wr16_le(ev + 2, ctx.transport().lastSeq());
                x11::wire::wr32_le(ev + 4, host);
                x11::wire::wr32_le(ev + 8, x11::atom::kWM_PROTOCOLS);
                x11::wire::wr32_le(ev + 12, x11::atom::kWM_TAKE_FOCUS);
                x11::wire::wr32_le(ev + 16, x11_now_ms_monotonic());
                (void)ctx.transport().sendEvent32(host, ev);
#ifndef NDEBUG
                TS_FPRINTF("[WM_TAKE_FOCUS] sent to host=0x%08X\n", (unsigned)host);
#endif
              }
#ifndef NDEBUG
              else {
                TS_FPRINTF("[WM_TAKE_FOCUS] suppressed bounce to host=0x%08X (prev=0x%08X last=0x%08X)\n",
                        (unsigned)host, (unsigned)ctx.input().take_focus_prev_, (unsigned)ctx.input().take_focus_last_);
              }
#endif
            }

            // Phase D (M9/M18): the WM's SetInputFocus(old → host) —
            // FocusOut(old) / FocusIn(host) at both levels with the
            // relation-derived detail (Nonlinear for two toplevels), each
            // delivered only to clients selecting FocusChange, exactly as
            // xorg does for a real WM.  Sent after WM_TAKE_FOCUS so a client
            // that answers the message with its own SetInputFocus sees the
            // same order it would under a WM.
            x11::focusev::doFocusEvents(ctx, srv->eventOps(), oldFocus, host,
                                        x11::notifymode::kNormal);

            // Check if macOS clipboard changed while we were in another app.
            // If so, claim PRIMARY+CLIPBOARD so the next paste serves macOS
            // content (prevents Xlib from short-circuiting ConvertSelection
            // when the requestor is also the selection owner).
            x11::SelectionOps::claimSelectionsIfMacOSChanged(ctx);

          } else {
            // Losing focus on this host — only act if this host actually
            // had focus.  A stale FocusOut for a destroyed/non-focused window
            // must not steal focus from the real focus holder.
            if (ctx.input().focus_host == host) {
              ctx.input().focus_host = 0;
              // NOTE: do NOT clear take_focus_prev_/last_ here — the bounce
              // detection depends on history surviving across loss/gain pairs.
              // Only button press (user interaction) clears the history.

              // Phase D: SetInputFocus(old → None): FocusOut(old, Nonlinear)
              // plus NonlinearVirtual on its ancestors, core + XI2.
              x11::focusev::doFocusEvents(ctx, srv->eventOps(), oldFocus, /*to=None*/0,
                                          x11::notifymode::kNormal);

              ctx.input().focus_xid = 0;
              // pointer_xid (the sprite window) is left alone: focus and the
              // pointer are independent, and AppKit's mouseExited on app
              // deactivation clears it through PointerLeave (v1.20.0.26).
            }
          }

          break;
        }

        // ------------------- Button
        case HostCmdType::Button: {
          ctx.input().mods = c.modsMask;

          const uint32_t host = c.xid ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // Update InputState position from this button event's coordinates,
          // so picking and event delivery use the actual click position
          // (not stale coords from the last PointerMove).
          ctx.input().win_x_u = c.win_x_u;
          ctx.input().win_y_u = c.win_y_u;
          ctx.input().root_x_u = c.root_x_u;
          ctx.input().root_y_u = c.root_y_u;

          // ---- macOS drag correction ----
          // macOS routes mouseUp to the original mouseDown window. If the
          // pointer is actually over a higher-stacking POPUP (override-
          // redirect menu/tooltip/combo), correct the host using root
          // coords from the last motion event.
          //
          // ONLY override-redirect windows are valid correction targets
          // (v1.19.36.24, matching the motion-path fix in .21).  The walk
          // uses X11 stacking order, which for NORMAL top-level windows is
          // stale relative to what the user sees (Cocoa owns their
          // Z-order).  Without this guard, a click on the FRONT window
          // (e.g. Vitis) was redirected to a normal window merely stacked
          // "above" it in X11's stale order (e.g. Vivado behind it) — the
          // click's action went to the wrong app.  macOS already delivers
          // the press to the correct normal window; only borderless popups
          // need rescuing.
          uint32_t effectiveHost = host;
          int32_t effectiveWinX = c.win_x_u;
          int32_t effectiveWinY = c.win_y_u;
          {
            const int32_t rx = ctx.input().root_x_u;
            const int32_t ry = ctx.input().root_y_u;
            auto topLevels = ctx.windows().childrenInStackOrder(1);
            for (auto it = topLevels.rbegin(); it != topLevels.rend(); ++it) {
              if (*it == host) break;
              x11::WindowView vw{};
              if (!ctx.windows().snapshot(*it, vw)) continue;
              if (!vw.mapped) continue;
              if (!vw.override_redirect) continue; // only popups may capture
              int32_t bw = (int32_t)vw.border_width;
              if (rx >= vw.x - bw && rx < vw.x + (int32_t)vw.w + bw &&
                  ry >= vw.y - bw && ry < vw.y + (int32_t)vw.h + bw) {
                effectiveHost = *it;
                effectiveWinX = rx - vw.x;
                effectiveWinY = ry - vw.y;
                break;
              }
            }
          }

          // ---- Active pointer grab: explicit (GrabPointer / XIGrabDevice) or
          // press-activated (implicit / passive — real grab records since
          // v1.20.0.18, xorg ActivateImplicitGrab / ActivatePassiveGrab).
          x11::PointerGrab activeGrab{};
          const bool haveActiveGrab = ctx.grabs().getPointerGrab(activeGrab) && activeGrab.active;

          // ---- STEP 1: Pick the deepest window BEFORE updating drag state ----
          // xorg always starts from the sprite window (the window under the
          // pointer); a grab only changes WHO the event is delivered to
          // (DeliverGrabbedEvent), decided below.  Picking first also lets
          // InputState::button() set drag_xid to the correct child.
          uint32_t under = x11::pickDeepestMappedWindowAtHostPoint(ctx, effectiveHost,
                                                                  effectiveWinX,
                                                                  effectiveWinY);
          if (!under) under = effectiveHost;

          // Raw event first (xorg fill_pointer_events emits the raw event
          // ahead of the device event, dix/getevents.c:1380-1395) — L20.
          srv->eventOps().sendXI2RawButtonEvent(ctx, c.isDown != 0, c.button);

          // ---- STEP 2: Passive grabs (GrabButton) on press — only while no
          // grab is active (xorg ProcessDeviceEvent: CheckDeviceGrabs runs
          // iff !grab, Xi/exevents.c:1917), checked root → sprite window so
          // an ancestor's grab wins (L9).  A match makes the grab window the
          // target; the passive record becomes the active grab below
          // (ActivatePassiveGrab).
          bool passiveMatched = false;
          x11::PassiveGrab pg{};
          if (c.isDown && !haveActiveGrab) {
            // GrabButton stores modifiers in X11 wire format (ControlMask=bit2).
            // c.modsMask uses internal format (Ctrl=bit1). Convert to X11.
            const uint16_t x11Mods = x11::input::toX11State(0, c.modsMask) & 0xFF;
            if (checkPassiveGrabsRootDown(ctx, under, effectiveHost, c.button, x11Mods, pg)) {
              passiveMatched = true;
              under = pg.grabWindow;
            }
          }

          // ---- STEP 3: Update button state with the CORRECT target ----
          // Save button state BEFORE the transition for the event state field.
          // X11 spec: "state is set to indicate the logical state just prior
          // to the event." For ButtonPress, state should NOT include the button
          // being pressed. For ButtonRelease, state should include it.
          const uint32_t buttonsBefore = ctx.input().buttons;

          // Now drag_xid will be set to 'under' (the child/grab window),
          // not the host. Subsequent button/motion events will route here.
          ctx.input().button(under, c.isDown != 0, c.button, c.buttonsMask);

          // [DRAG] session bracketing.  begin() captures host/drag_xid the
          // moment the 0→nonzero transition lands (so InputState::button
          // has already populated drag_xid).  end() emits the counter +
          // duration on the matching release.  No effect when the trace
          // category is disabled (compiles to nothing).
          if (c.isDown) {
            x11::drag_trace::begin(effectiveHost,
                                   ctx.input().drag_xid,
                                   c.button);
          } else {
            x11::drag_trace::end(c.button);
          }

          // NOTE: No click-to-focus here. Focus is handled by the
          // HostCmdType::Focus handler (Cocoa becomeKey/resignKey) which
          // sends FocusIn to the HOST. The toolkit (Xt) then propagates
          // to children via SetInputFocus.

          // Pick a delivery window that selected the relevant mask.
          // xorg EventIsDeliverable (dix/events.c): a window is a delivery
          // target if it selected the event at ANY level — XI2 (its own
          // XISelectEvents mask) or core.  GTK3 under XI2 selects buttons
          // ONLY via XI2 and never sets the core ButtonPressMask; checking
          // core alone made every GTK click fall through to [BTN_DROP]
          // before either sender ran (dialog got XI2 motion but no clicks,
          // so double-click-to-open-folder could never fire).
          auto wantsBtn = [&](uint32_t xid) -> bool {
            if (!xid) return false;
            const x11::WindowView* vw = ctx.window(xid);
            if (!vw || vw->owner_fd <= 0) return false;
            const uint32_t mask = vw->event_mask;
            const bool coreWant = c.isDown ? (mask & x11::mask::ButtonPress)   != 0
                                           : (mask & x11::mask::ButtonRelease) != 0;
            const uint32_t xi2bit = c.isDown ? x11::xi2::kButtonPressMask
                                             : x11::xi2::kButtonReleaseMask;
            const bool xi2Want = (vw->xi2_mask & xi2bit) != 0;
            return coreWant || xi2Want;
          };

          uint32_t deliver = under;

          // If under doesn't select, climb to parent until effective host (simple propagation).
          // do_not_propagate_mask (§2.8): propagation from a window stops
          // when the event's mask bit is in that window's dnp mask.
          const uint32_t btnDnpBit = c.isDown ? x11::mask::ButtonPress
                                              : x11::mask::ButtonRelease;
          bool btnPropagationFenced = false;
          if (!wantsBtn(deliver)) {
            uint32_t cur = under;
            int safety = 0;
            while (cur && cur != effectiveHost) {
              x11::WindowView vw{};
              if (!ctx.windows().snapshot(cur, vw)) break;
              if (vw.do_not_propagate_mask & btnDnpBit) {
                btnPropagationFenced = true;
                break;
              }
              cur = vw.parent_xid;
              if (wantsBtn(cur)) { deliver = cur; break; }
              if (++safety > 64) break;
            }
            // If still not found, try effective host last (unless fenced).
            if (!btnPropagationFenced &&
                !wantsBtn(deliver) && wantsBtn(effectiveHost)) deliver = effectiveHost;
          }

          const bool normalWants = wantsBtn(deliver);

          // ---- Grab routing: xorg DeliverGrabbedEvent (dix/events.c:4399-4434) ----
          // owner_events: normal delivery, but only within the grabbing
          // client's windows (TryClientEvents returns -1 for anyone else and
          // the walk stops, :2039-2044, :2892-2896); otherwise the grab window
          // at the grab's level with the grab's mask, addressed to the
          // grabbing client.  A click on Electron while a GTK popup holds an
          // owner_events grab therefore reaches the popup, not Electron (G-3).
          //
          // C1: a passive GrabButton matched by THIS press routes the same way
          // (xorg ActivatePassiveGrab, dix/events.c:3854-3862, activates the
          // grab and delivers the triggering press to rClient(grab) at the grab
          // window — never gated by what the window selected).  The click path
          // used to run the press through window selection and drop it at
          // [BTN_DROP] when nobody had selected ButtonPress, so Java's
          // XGrabButton fallback / Motif bindings / click-to-raise never fired.
          // The wheel path (ScrollTicks, L8) already routed passive matches
          // this way; the two paths now agree.
          x11::PointerGrab routeGrab{};
          bool haveRouteGrab = false;
          if (haveActiveGrab) {
            routeGrab = activeGrab;
            haveRouteGrab = true;
          } else if (passiveMatched) {
            routeGrab.active      = true;
            routeGrab.grabWindow  = pg.grabWindow;
            routeGrab.ownerEvents = pg.ownerEvents;
            routeGrab.eventMask   = pg.eventMask;
            routeGrab.is_xi2      = false;
            routeGrab.owner_fd    = pg.owner_fd;   // C1: rClient(grab)
            routeGrab.implicit    = true;
            haveRouteGrab = true;
          }

          uint32_t target  = deliver;
          bool     viaGrab = false;
          int      toFd    = -1;
          if (haveRouteGrab) {
            const auto d = x11::grabroute::route(ctx, routeGrab, normalWants ? deliver : 0);
            target  = d.target;
            viaGrab = d.viaGrab;
            if (viaGrab) toFd = routeGrab.owner_fd;
          } else if (!normalWants) {
            // Diagnostic: log why the click was dropped
            const x11::WindowView* dbgUnder = ctx.window(under);
            const x11::WindowView* dbgHost  = ctx.window(effectiveHost);
            TS_FPRINTF("[BTN_DROP] host=0x%08X under=0x%08X "
                       "host_mask=0x%08X host_fd=%d "
                       "under_mask=0x%08X under_fd=%d "
                       "pos=(%d,%d) btn=%d %s\n",
                       (unsigned)effectiveHost, (unsigned)under,
                       dbgHost ? dbgHost->event_mask : 0u,
                       dbgHost ? dbgHost->owner_fd : -1,
                       dbgUnder ? dbgUnder->event_mask : 0u,
                       dbgUnder ? dbgUnder->owner_fd : -1,
                       (int)effectiveWinX, (int)effectiveWinY,
                       (int)c.button, c.isDown ? "DOWN" : "UP");
            break;
          }

          // Implicit-grab target (review 2026-08-31 §2.6): the spec's
          // automatic grab belongs to the window the press was DELIVERED
          // to, not the pre-propagation pick.  InputState::button() above
          // set drag_xid = under; retarget to deliver so drag motion
          // routes to the window that actually received the ButtonPress.
          // (A passive match already set under = grab window.)
          if (!haveActiveGrab && !passiveMatched && c.isDown &&
              ctx.input().drag_xid == under && deliver != under) {
            ctx.input().drag_xid = deliver;
          }

          // child field: per X11 spec, "child is set to the child of the
          // event window that is the ancestor of (or is) the source window."
          // xorg computes it for grabbed delivery too — FixUpEventFromWindow
          // (grab->window, None, calcChild=TRUE), dix/events.c:4357 — so a
          // root-window grab sees the toplevel under the pointer; AWT's XDND
          // reads that as the drop-target candidate (v1.20.0.19).
          const uint32_t child = x11::grabroute::childOnSpritePath(ctx, target, under);


#ifndef NDEBUG
          {
            const x11::WindowView* dbgDel = ctx.window(target);
            TS_FPRINTF("[BTN_SEND] host=0x%08X under=0x%08X deliver=0x%08X "
                       "del_fd=%d del_mask=0x%08X child=0x%08X "
                       "pos=(%d,%d) btn=%d %s%s\n",
                       (unsigned)effectiveHost, (unsigned)under,
                       (unsigned)target,
                       dbgDel ? dbgDel->owner_fd : -1,
                       dbgDel ? dbgDel->event_mask : 0u,
                       (unsigned)child,
                       (int)effectiveWinX, (int)effectiveWinY,
                       (int)c.button, c.isDown ? "DOWN" : "UP",
                       viaGrab ? " via-grab" : "");
          }
#endif
          const int32_t rx = ctx.input().root_x_u, ry = ctx.input().root_y_u;
          bool deliveredXI2 = false;
          if (viaGrab) {
            // DeliverOneGrabbedEvent (dix/events.c:4322-4366): the grab's level
            // only, filtered by the grab's own mask, to the grabbing client.
            const uint32_t xi2bit  = c.isDown ? x11::xi2::kButtonPressMask : x11::xi2::kButtonReleaseMask;
            const uint32_t corebit = c.isDown ? x11::mask::ButtonPress      : x11::mask::ButtonRelease;
            if (x11::grabroute::grabWantsXI2(routeGrab, xi2bit)) {
              (void)srv->eventOps().sendXI2ButtonEvent(ctx, target, c.isDown != 0, c.button,
                                                       rx, ry, buttonsBefore, c.modsMask,
                                                       child, /*force=*/true, toFd);
            } else if (x11::grabroute::grabWantsCore(routeGrab, corebit)) {
              srv->eventOps().sendButtonEvent(ctx, target, c.isDown != 0, c.button,
                                              rx, ry, buttonsBefore, c.modsMask,
                                              child, toFd);
            }
          } else {
            // xorg DeliverDeviceEvents: XI2 first; if it delivers via the window's
            // own selection, the core event is suppressed (no double-processing).
            deliveredXI2 =
              srv->eventOps().sendXI2ButtonEvent(ctx, target,
                                                 c.isDown != 0, c.button,
                                                 rx, ry, buttonsBefore, c.modsMask,
                                                 child);
            if (!deliveredXI2) {
              srv->eventOps().sendButtonEvent(ctx, target,
                                              c.isDown != 0, c.button,
                                              rx, ry, buttonsBefore, c.modsMask,
                                              child);
            }
          }

          // xorg ActivateImplicitGrab (dix/events.c:2119-2164) /
          // ActivatePassiveGrab (:3854-3863): a delivered press with no active
          // grab starts a grab — on the delivery window with that window's mask
          // (owner_events from OwnerGrabButtonMask), or the matched passive
          // record owned by rClient(grab) (C1).  ActivatePointerGrab sends
          // Leave(sprite)/Enter(grab window) with NotifyGrab when they differ
          // (M17).  Runs after delivery in BOTH branches: a passive match is
          // delivered via the grab, so this used to be skipped (the passive
          // grab was recorded, but only in the normal-delivery branch).
          if (c.isDown && !haveActiveGrab) {
            x11::PointerGrab ig{};
            if (passiveMatched) {
              ig.grabWindow  = pg.grabWindow;
              ig.ownerEvents = pg.ownerEvents;
              ig.eventMask   = pg.eventMask;
              ig.owner_fd    = pg.owner_fd;   // C1: the grabbing client
              ig.is_xi2      = false;
            } else {
              const x11::WindowView* dv = ctx.window(target);
              if (dv) {
                ig.grabWindow  = target;
                ig.ownerEvents = (dv->event_mask & x11::mask::OwnerGrabButton) != 0;
                ig.eventMask   = (uint16_t)dv->event_mask;
                ig.owner_fd    = dv->owner_fd;
                ig.is_xi2      = deliveredXI2;
                ig.xi2mask     = dv->xi2_mask;
                // Phase C: the grab belongs to the client that RECEIVED the
                // press (xorg ActivateImplicitGrab records `client` and its
                // own mask on the window), which with per-client selections
                // need not be the window's owner.
                if (deliveredXI2) {
                  int rfd = -1; uint32_t rmask = 0;
                  if (ctx.windows().firstXI2Selector(target, x11::xi2::kButtonPressMask,
                                                     x11::xi2::kVirtualCorePointer, rfd, rmask)) {
                    ig.owner_fd = rfd;
                    ig.xi2mask  = rmask;
                  }
                } else {
                  const std::vector<int> cfds = ctx.windows().selectorsOf(target, x11::mask::ButtonPress);
                  if (!cfds.empty() &&
                      std::find(cfds.begin(), cfds.end(), dv->owner_fd) == cfds.end()) {
                    ig.owner_fd = cfds.front();
                  }
                }
              }
            }
            ig.grab_time = x11_now_ms_monotonic();
            ig.implicit  = true;
            if (ig.grabWindow && ig.owner_fd >= 0 &&
                ctx.grabs().tryPointerGrab(ig) == x11::kGrabSuccess) {
              x11::grabchoreo::pointerGrabCrossings(ctx, srv->eventOps(), under, ig.grabWindow, /*NotifyGrab*/1);
            }
          }

          // xorg ProcessDeviceEvent (Xi/exevents.c:1927-1930, 1949-1950): a
          // press-activated grab is released on the last button-up, AFTER the
          // release is delivered; DeactivatePointerGrab then sends
          // Leave(grab window)/Enter(sprite window) with NotifyUngrab.
          if (!c.isDown && haveActiveGrab && activeGrab.implicit && ctx.input().buttons == 0) {
            ctx.grabs().clearPointerGrab(-1);
            x11::grabchoreo::pointerGrabCrossings(ctx, srv->eventOps(), activeGrab.grabWindow, under, /*NotifyUngrab*/2);
          }
          break;
        }


        // ------------------- ScrollTicks
        case HostCmdType::ScrollTicks: {
          const uint32_t host = c.xid ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // Horizontal ticks are buttons 6/7, which the core pointer
          // advertises (dix/devices.c:660-661) — delivered since Phase G.

          const int32_t rx = ctx.input().root_x_u;
          const int32_t ry = ctx.input().root_y_u;

          ctx.input().updateMotion(host,
                                   c.win_x_u, c.win_y_u,
                                   rx, ry,
                                   ctx.input().buttons,
                                   c.modsMask);

          const uint32_t under  = x11::pickDeepestMappedWindowAtHostPoint(ctx, host, c.win_x_u, c.win_y_u);
          const uint32_t target = under ? under : host;

          // Cursor: apply to host, choose based on routed pointer target.
          const uint32_t cursorTarget = ctx.input().routePointer(target);
          maybeApplyCursor(ctx, host, cursorTarget);

          auto wheelButton = [&](uint8_t axis, int16_t ticks) -> uint8_t {
            if (axis == 0) return (ticks > 0) ? 4 : 5; // vertical up/down
            else           return (ticks > 0) ? 6 : 7; // horizontal right/left
          };

          const int16_t ticks = (int16_t)c.ticks;
          const int n   = (ticks >= 0) ? (int)ticks : (int)(-ticks);
          const int dir = (ticks >= 0) ? +1 : -1;

          const int nClamped = (n > 64) ? 64 : n;

          // Wheel buttons are ButtonPress/Release and take exactly the path
          // of any button (xorg Xi/exevents.c:1913-1930): an active grab
          // routes them (DeliverGrabbedEvent); otherwise a passive grab on
          // the wheel button along the sprite trace activates for the
          // press/release pair (CheckDeviceGrabs), and normal delivery
          // climbs by selection from the window under the pointer
          // (DeliverDeviceEvents).  Before Phase G (L8) the event went to
          // that window's owner even when nobody had selected it, and
          // passive GrabButton(4..7) records were never consulted.
          x11::PointerGrab sGrab{};
          const bool haveSGrab = ctx.grabs().getPointerGrab(sGrab) && sGrab.active;
          const uint16_t x11ModsW = x11::input::toX11State(0, c.modsMask) & 0xFF;

          // Deliver one wheel press or release to `to`, through `g` when viaGrab.
          auto sendWheelTo = [&](bool press, uint8_t btn, uint32_t btnState,
                                 uint32_t to, bool viaGrab, int toFd, const x11::PointerGrab& g,
                                 uint32_t child) {
            if (viaGrab) {
              const uint32_t xi2bit  = press ? x11::xi2::kButtonPressMask : x11::xi2::kButtonReleaseMask;
              const uint32_t corebit = press ? x11::mask::ButtonPress      : x11::mask::ButtonRelease;
              if (x11::grabroute::grabWantsXI2(g, xi2bit))
                (void)srv->eventOps().sendXI2ButtonEvent(ctx, to, press, btn, rx, ry,
                                                         btnState, c.modsMask, child, /*force=*/true, toFd);
              else if (x11::grabroute::grabWantsCore(g, corebit))
                srv->eventOps().sendButtonEvent(ctx, to, press, btn, rx, ry,
                                                btnState, c.modsMask, child, toFd);
              return;
            }
            if (!srv->eventOps().sendXI2ButtonEvent(ctx, to, press, btn, rx, ry,
                                                    btnState, c.modsMask, child)) {
              srv->eventOps().sendButtonEvent(ctx, to, press, btn, rx, ry,
                                              btnState, c.modsMask, child);
            }
          };

          for (int i = 0; i < nClamped; i++) {
            const uint8_t btn = wheelButton(c.axis, (int16_t)dir);
            const uint32_t wheelMask = (btn >= 1 && btn <= 31) ? (1u << (btn - 1u)) : 0;

            // Raw events precede delivery (L20).
            srv->eventOps().sendXI2RawButtonEvent(ctx, true, btn);

            uint32_t to = 0; bool viaGrab = false; int toFd = -1;
            x11::PointerGrab g = sGrab;
            const uint32_t normal = buttonDeliveryWindow(ctx, under, host, true);
            if (haveSGrab) {
              const auto d = x11::grabroute::route(ctx, sGrab, normal);
              to = d.target; viaGrab = d.viaGrab;
              if (viaGrab) toFd = sGrab.owner_fd;
            } else {
              x11::PassiveGrab pg{};
              if (checkPassiveGrabsRootDown(ctx, under, host, btn, x11ModsW, pg)) {
                g = x11::PointerGrab{};
                g.active = true; g.grabWindow = pg.grabWindow; g.ownerEvents = pg.ownerEvents;
                g.eventMask = pg.eventMask; g.is_xi2 = false; g.implicit = true;
                g.owner_fd = pg.owner_fd;   // C1: rClient(grab), not the grab window's owner
                const auto d = x11::grabroute::route(ctx, g, normal);
                to = d.target; viaGrab = d.viaGrab;
                if (viaGrab) toFd = g.owner_fd;
              } else {
                to = normal;
              }
            }
            if (!to) {
              srv->eventOps().sendXI2RawButtonEvent(ctx, false, btn);
              continue;   // nobody selected the wheel button (or fenced)
            }
            const uint32_t child = x11::grabroute::childOnSpritePath(ctx, to, under);

        #ifdef X11_TRACE_VERBOSE
            fprintf(stderr,
                    "[SCROLL] host=0x%08X target=0x%08X axis=%u ticks=%d btn=%u win=(%d,%d) root=(%d,%d) grab=%d t=%u\n",
                    (unsigned)host, (unsigned)to,
                    (unsigned)c.axis, (int)ticks, (unsigned)btn,
                    (int)c.win_x_u, (int)c.win_y_u,
                    (int)rx, (int)ry, (int)viaGrab,
                    (unsigned)x11_now_ms_monotonic());
        #endif

            sendWheelTo(true, btn, ctx.input().buttons, to, viaGrab, toFd, g, child);
            srv->eventOps().sendXI2RawButtonEvent(ctx, false, btn);
            sendWheelTo(false, btn, ctx.input().buttons | wheelMask, to, viaGrab, toFd, g, child);
          }

          // NOTE: Do NOT send Expose after scroll events. xterm handles
          // its own drawing (CopyArea + FillRectangle) in response to
          // button 4/5. A spurious full-window Expose triggers redundant
          // clear+redraw that races with the scrollbar thumb update,
          // causing the scrollbar to vanish.
          break;
        }


        // ------------------- Key
        case HostCmdType::Key: {
          const uint32_t host = (c.xid != 0) ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // Clamp X11 keycode: mac_vk + 8, range [8..255]
          uint32_t kc32 = (uint32_t)c.keyCode + 8u;
          if (kc32 < 8u)   kc32 = 8u;
          if (kc32 > 255u) kc32 = 255u;
          const uint8_t x11_kc = (uint8_t)kc32;

          // Track key state for QueryKeymap
          if (c.isDown) ctx.input().keyDown(x11_kc);
          else          ctx.input().keyUp(x11_kc);

          // Raw event first (xorg GetKeyboardEvents, dix/getevents.c:1117) — L20.
          srv->eventOps().sendXI2RawKeyEvent(ctx, c.isDown != 0, x11_kc);

          // xorg event_set_state (dix/inpututils.c:794-796): a key event
          // carries the modifier state BEFORE the key — for a modifier key
          // that is the previous canonical state (Phase E, M11).  Cocoa sends
          // the post-event flags in modsMask; those become the new canonical
          // state, the event is stamped with the old one.
          const uint32_t evMods = ctx.input().mods;
          ctx.input().mods = c.modsMask;
          const bool isRepeat = (c.isRepeat != 0);   // XI2 XIKeyRepeat flag

          // xorg EventIsDeliverable: deliverable via the window's own XI2 mask
          // OR its core mask.  GTK3 under XI2 selects keys only via XI2, so
          // a core-only check dropped typing in GTK dialogs.
          auto wantsKey = [&](uint32_t xid) -> bool {
            if (!xid) return false;
            const x11::WindowView* vw = ctx.window(xid);
            if (!vw || vw->owner_fd <= 0) return false;
            const uint32_t mask = vw->event_mask;
            const bool coreWant = c.isDown ? (mask & x11::mask::KeyPress)   != 0
                                           : (mask & x11::mask::KeyRelease) != 0;
            const uint32_t xi2bit = c.isDown ? x11::xi2::kKeyPressMask
                                             : x11::xi2::kKeyReleaseMask;
            const bool xi2Want = (vw->xi2_mask & xi2bit) != 0;
            return coreWant || xi2Want;
          };

          // Ungrabbed target — xorg DeliverFocusedEvent (dix/events.c:
          // 4239-4299), Phase D (M19):
          //   focus == PointerRoot  → normal delivery walking up from the
          //                           sprite (pointer) window;
          //   focus is, or is an ancestor of, the sprite window
          //                         → walk up from the sprite window,
          //                           stopping at the focus window;
          //   otherwise             → the focus window only, no propagation.
          // Each step climbs by selection (core or XI2), fenced by
          // do_not_propagate (§2.8).  Deviations kept from the Cocoa focus
          // model: focus None (xorg drops the key) and a focus that lives in
          // another host while THIS NSWindow is key deliver to this host.
          // The result is also stage 1 of an owner_events keyboard grab.
          const uint32_t focus = ctx.input().focus_xid;
          uint32_t sprite = ctx.input().pointer_xid;
          if (sprite && !ctx.window(sprite)) sprite = 0;
          const uint32_t keyDnpBit = c.isDown ? x11::mask::KeyPress
                                              : x11::mask::KeyRelease;
          uint32_t start = host, stopAt = host;
          if (focus == x11::kRootXid) {                         // PointerRoot
            start = sprite ? sprite : host; stopAt = 0;
          } else if (focus == 0) {
            start = host; stopAt = host;
          } else if (focus != host && ctx.windows().topLevelAncestorOf(focus) != host) {
            start = host; stopAt = host;                        // focus elsewhere; Cocoa says here
          } else if (sprite && (sprite == focus || x11::wintree::isAncestor(ctx, focus, sprite))) {
            start = sprite; stopAt = focus;
          } else {
            start = focus; stopAt = focus;                      // focus window only
          }
          uint32_t target = 0;
          {
            uint32_t cur = start;
            for (int safety = 0; cur && cur != x11::kRootXid && safety < 64; safety++) {
              if (wantsKey(cur)) { target = cur; break; }
              if (cur == stopAt) break;
              x11::WindowView vw{};
              if (!ctx.windows().snapshot(cur, vw)) break;
              if (vw.do_not_propagate_mask & keyDnpBit) break;
              cur = vw.parent_xid;
            }
          }
          const bool normalWants = target != 0;

#ifndef NDEBUG
          {
            x11::KeyboardGrab dbgKg{};
            const bool dbgHave = ctx.grabs().getKeyboardGrabInfo(dbgKg) && dbgKg.active;
            const x11::WindowView* dbgT = ctx.window(target);
            TS_FPRINTF("[KEY_ROUTE] host=0x%08X focus=0x%08X target=0x%08X t_fd=%d t_mask=0x%08X t_xi2=0x%08X "
                       "wants=%d kc=%u %s mods=0x%X kbgrab=%s win=0x%08X fd=%d xi2=%d owner_ev=%d\n",
                       (unsigned)host, (unsigned)focus, (unsigned)target,
                       dbgT ? dbgT->owner_fd : -1, dbgT ? dbgT->event_mask : 0u, dbgT ? dbgT->xi2_mask : 0u,
                       (int)normalWants, (unsigned)x11_kc, c.isDown ? "DOWN" : "UP", (unsigned)c.modsMask,
                       dbgHave ? "yes" : "no", (unsigned)dbgKg.grabWindow, dbgKg.owner_fd,
                       (int)dbgKg.is_xi2, (int)dbgKg.ownerEvents);
          }
#endif

          // ---- C2: passive keyboard grab (GrabKey) activation.  On a key
          // press with no keyboard grab active, xorg CheckDeviceGrabs walks the
          // focus trace root-down (dix/events.c:4183-4206) and, on a match,
          // ActivatePassiveGrab activates a keyboard grab on the grab window
          // owned by the grabbing client (dix/events.c:3854-3862) and delivers
          // the press to it.  The grab terminates on the activating key's
          // release.  Hotkey clients (WM accelerators, Swing/GTK mnemonics)
          // never fired before this — GrabKey was a pure no-op.
          if (c.isDown) {
            x11::KeyboardGrab existing{};
            const bool haveKb = ctx.grabs().getKeyboardGrabInfo(existing) && existing.active;
            if (!haveKb) {
              const uint16_t x11ModsKey = x11::input::toX11State(0, evMods) & 0xFFu;
              x11::PassiveKeyGrab kpg{};
              if (checkPassiveKeyGrabsRootDown(ctx, focus, sprite, x11_kc, x11ModsKey, kpg)) {
                x11::KeyboardGrab kgg{};
                kgg.grabWindow  = kpg.grabWindow;
                kgg.ownerEvents = kpg.ownerEvents;
                kgg.owner_fd    = kpg.owner_fd;
                kgg.grab_time   = x11_now_ms_monotonic();
                kgg.is_xi2      = false;
                kgg.implicit    = true;
                kgg.grab_key    = x11_kc;
                if (ctx.grabs().tryKeyboardGrab(kgg) == x11::kGrabSuccess) {
                  // ActivateKeyboardGrab (dix/events.c:1720-1735): FocusOut
                  // (focus → grab window) / FocusIn NotifyGrab at both levels.
                  const uint32_t from = ctx.input().focus_xid;
                  if (from && from != kpg.grabWindow)
                    x11::grabchoreo::keyboardGrabFocusPair(ctx, srv->eventOps(),
                                                           from, kpg.grabWindow, /*NotifyGrab*/1);
                }
              }
            }
          }

          // ---- Active keyboard grab (GrabKeyboard / XIGrabDevice on the
          // keyboard, or a passive GrabKey just activated above) — xorg
          // DeliverGrabbedEvent: with owner_events the focus-based target if it
          // belongs to the grabbing client, otherwise the grab window at the
          // grab's level (a core GrabKeyboard's mask is KeyPress|KeyRelease,
          // dix/events.c:5317; an XI2 grab uses its xi2mask), addressed to the
          // grabbing client.  Swing popups/combos and GTK menus grab the
          // keyboard for arrow/Escape navigation.
          bool consumedByGrab = false;
          x11::KeyboardGrab kg{};
          const bool haveKbGrab = ctx.grabs().getKeyboardGrabInfo(kg) && kg.active &&
                                  (kg.grabWindow == x11::kRootXid || ctx.window(kg.grabWindow));
          if (haveKbGrab) {
            bool viaGrab = true;
            if (kg.ownerEvents && normalWants) {
              const x11::WindowView* tv = ctx.window(target);
              if (tv && tv->owner_fd == kg.owner_fd) viaGrab = false;
            }
            if (viaGrab) {
              const uint32_t xi2bit = c.isDown ? x11::xi2::kKeyPressMask : x11::xi2::kKeyReleaseMask;
              if (kg.is_xi2) {
                if (kg.xi2mask & xi2bit)
                  (void)srv->eventOps().sendXI2KeyEvent(ctx, kg.grabWindow, c.isDown != 0, x11_kc,
                                                        ctx.input().buttons, evMods,
                                                        /*force=*/true, kg.owner_fd, isRepeat);
              } else {
                srv->eventOps().sendKeyEvent(ctx, kg.grabWindow, c.isDown != 0, x11_kc,
                                             ctx.input().buttons, evMods, kg.owner_fd);
              }
              consumedByGrab = true;
            }
            // else owner_events within the grabbing client: normal delivery.
          }

          // Normal delivery — skipped when the grab consumed the event, or when
          // there is no grab and nobody selected the key (or propagation was
          // fenced).
          if (!consumedByGrab && (haveKbGrab || normalWants)) {
        #ifdef X11_TRACE_VERBOSE
            fprintf(stderr,
                    "[KEY] host=0x%08X focus=0x%08X deliver=0x%08X down=%d kc=%u mods=0x%X\n",
                    (unsigned)host, (unsigned)focus, (unsigned)target,
                    (int)(c.isDown != 0), (unsigned)x11_kc, (unsigned)c.modsMask);
        #endif
            if (!srv->eventOps().sendXI2KeyEvent(ctx, target,
                                                 c.isDown != 0,
                                                 x11_kc,
                                                 ctx.input().buttons, evMods,
                                                 /*force=*/false, /*toFd=*/-1, isRepeat)) {
              srv->eventOps().sendKeyEvent(ctx, target,
                                           c.isDown != 0,
                                           x11_kc,
                                           ctx.input().buttons, evMods);
            }
          }

          // C2: a passive-GrabKey-activated grab terminates on the release of
          // the key that activated it (X11 GrabKey), after the release has been
          // delivered.  DeactivateKeyboardGrab sends FocusOut(grab window) /
          // FocusIn(focus) NotifyUngrab.
          if (!c.isDown) {
            x11::KeyboardGrab rk{};
            if (ctx.grabs().getKeyboardGrabInfo(rk) && rk.active && rk.implicit &&
                rk.grab_key == x11_kc) {
              const uint32_t gw = ctx.grabs().clearKeyboardGrab(rk.owner_fd);
              if (gw)
                x11::grabchoreo::keyboardGrabFocusPair(ctx, srv->eventOps(), gw,
                                                       ctx.input().focus_xid, /*NotifyUngrab*/2);
            }
          }
          break;
        }

        // ------------------- ClipboardCapture (removed v1.15.8)
        // Proactive clipboard capture was removed because it deadlocks when
        // the client's event thread is blocked (Java Swing menu tracking,
        // text selection callbacks).  X11→macOS sync now happens lazily via
        // handleSendEvent's SelectionNotify interception.
        case HostCmdType::ClipboardCapture:
          break;

        // ------------------- WindowClose
        // Handled directly in XProtoDaemon::drainHostCommands() (needs
        // access to removeClient).  Listed here to silence -Wswitch.
        case HostCmdType::WindowClose:
          break;

        // ------------------- ScreenLayoutChanged
        // Handled directly in XProtoDaemon::drainHostCommands() (needs
        // iteration over all clients).  Listed here to silence -Wswitch.
        case HostCmdType::ScreenLayoutChanged:
          break;

        case HostCmdType::WindowMoved:
          // Handled in XProtoDaemon::drainHostCommands (needs per-client transport)
          break;

  } // switch
}


// Callable from daemon (processes individual commands with correct client).
extern "C" void x11_proto_bridge_process_host_cmd(const void* cmd_ptr)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  const auto& c = *reinterpret_cast<const x11::HostCmd*>(cmd_ptr);
  processOneHostCmd(srv, srv->ctx(), c);
}


extern "C" void x11_proto_bridge_flush_notify_queue(void)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;

  auto& ctx = srv->ctx();

  // Drain host commands on xproto thread.
  {
    auto cmds = srv->hostCmds().takeAll();
    for (const auto& c : cmds) {
      processOneHostCmd(srv, ctx, c);
    }
  }

  srv->flushNotifyQueue();
}

//extern "C" void x11_proto_bridge_queue_notify(uint32_t wid, int want_configure, int want_expose)
//{
//  auto* srv = x11_proto_bridge_get_server();
//  if (!srv) return;
//  srv->queueNotify(wid, want_configure != 0, want_expose != 0);
//}

extern "C" void x11_proto_bridge_queue_expose_rect(uint32_t wid,
                                                   uint16_t x, uint16_t y,
                                                   uint16_t w, uint16_t h,
                                                   uint16_t count) {
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  if (!srv->ctx().hasClient()) return;
  srv->ctx().transport().queueExposeRect(wid, x, y, w, h, count);
}

extern "C" int x11_proto_bridge_send_reply_bytes(const void* buf, size_t n)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return 0;
  if (!buf || n == 0) return 0;

  // Must be on xproto thread; transport enforces that.
  if (!srv->ctx().hasClient()) return 0;
  return srv->ctx().transport().sendReplyBytes(buf, n) ? 1 : 0;
}

//extern "C" int x11_proto_bridge_send_get_geometry_reply(uint16_t seq,
//                                                        uint32_t root,
//                                                        int16_t x, int16_t y,
//                                                        uint16_t w, uint16_t h,
//                                                        uint16_t borderWidth,
//                                                        uint16_t depth)
//{
//  auto* srv = x11_proto_bridge_get_server();
//  if (!srv) return 0;
//
//  // Forward to the unified ReplyWriter path.
//  return srv->ctx().reply().sendGetGeometryReply(seq, root, x, y, w, h, borderWidth, depth) ? 1 : 0;
//}

// extern "C" int x11_proto_bridge_send_get_input_focus_reply(uint16_t seq,
//                                                            uint8_t revertTo,
//                                                            uint32_t focus)
// {
//   auto* srv = x11_proto_bridge_get_server();
//   if (!srv) return 0;
//
//   return srv->ctx().reply().sendGetInputFocusReply(seq, revertTo, focus) ? 1 : 0;
// }


extern "C" int x11_proto_bridge_send_intern_atom_reply(uint16_t seq, uint32_t atom) {
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return 0;
  return srv->ctx().reply().sendInternAtomReply(seq, atom) ? 1 : 0;
}

extern "C" int x11_proto_bridge_send_get_atom_name_reply(uint16_t seq, const char* name, uint16_t nameLen) {
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return 0;
  return srv->ctx().reply().sendGetAtomNameReply(seq, name, nameLen) ? 1 : 0;
}


extern "C" int x11_proto_bridge_dispatch(uint8_t major, uint8_t minor, uint16_t seq,
                                         const uint8_t* payload, size_t remain)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return 0;
  return srv->dispatch(major, minor, seq, payload, remain) ? 1 : 0;
}


extern "C" void x11_proto_bridge_window_erase(uint32_t xid)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  srv->ctx().windows().erase(xid);
}

extern "C" void x11_proto_bridge_window_set_mapped(uint32_t xid, int mapped)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  srv->ctx().windows().setMapped(xid, mapped != 0);
}

extern "C" void x11_proto_bridge_window_set_event_mask(uint32_t xid, uint32_t event_mask)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  srv->ctx().windows().setEventMask(xid, event_mask);
}

//extern "C" void x11_proto_bridge_window_set_geometry(uint32_t xid, int16_t x, int16_t y, uint16_t w, uint16_t h)
//{
//  auto* srv = x11_proto_bridge_get_server();
//  if (!srv) return;
//  srv->ctx().windows().setGeometry(xid, x, y, w, h);
//}

extern "C" int x11_proto_bridge_window_is_ready_to_present(uint32_t xid)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return 0;
  return srv->ctx().windows().isReadyToPresent(xid) ? 1 : 0;
}

extern "C" void x11_proto_bridge_window_mark_dirty(uint32_t xid)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  srv->ctx().windows().markDirty(xid);
}

extern "C" void x11_proto_bridge_window_debug_state(uint32_t xid,
                                                    uint32_t* out_parent,
                                                    int* out_mapped,
                                                    int* out_presentable,
                                                    int* out_dirty,
                                                    int* out_owner_fd)
{
  if (out_parent)      *out_parent = 0;
  if (out_mapped)      *out_mapped = 0;
  if (out_presentable) *out_presentable = 0;
  if (out_dirty)       *out_dirty = 0;
  if (out_owner_fd)    *out_owner_fd = -1;

  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;

  x11::WindowView vw{};
  if (!srv->ctx().windows().snapshot(xid, vw)) {
    // fallback to old callback snapshot if you want:
    // if (!srv->ctx().snapshotViaCallback(xid, vw)) return;
    return;
  }

  if (out_parent)      *out_parent = vw.parent_xid;     // see note below
  if (out_mapped)      *out_mapped = vw.mapped ? 1 : 0;
  if (out_presentable) *out_presentable = vw.presentable ? 1 : 0; // see note below
  if (out_dirty)       *out_dirty = vw.dirty ? 1 : 0;             // see note below
  if (out_owner_fd)    *out_owner_fd = vw.owner_fd;
}

extern "C" void x11_proto_bridge_pixmap_create(uint32_t pid, uint8_t depth, uint16_t w, uint16_t h)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  srv->ctx().pixmaps().createPixmap(pid, depth, w, h);
}

extern "C" void x11_proto_bridge_pixmap_free(uint32_t pid)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  srv->ctx().pixmaps().freePixmap(pid);
}

extern "C" void x11_proto_bridge_apply_rootless_resize(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (xid == 0) return;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  srv->hostCmds().push(HostCmd{HostCmdType::RootlessResize, xid, w_px, h_px});
}

extern "C" void x11_proto_bridge_window_set_presentable_and_flush(uint32_t xid)
{
  if (xid == 0) return;
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  srv->hostCmds().push(HostCmd{HostCmdType::SetPresentable, xid, 0, 0});
}

extern "C" void x11_proto_bridge_surface_resized(uint32_t xid, int32_t in_live_resize)
{
  if (xid == 0) return;
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  // live-resize flag rides in w_px.
  srv->hostCmds().push(HostCmd{HostCmdType::SurfaceResized, xid, in_live_resize, 0});
}

extern "C" void x11_proto_bridge_expose_children(uint32_t xid)
{
  if (xid == 0) return;
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  srv->hostCmds().push(HostCmd{HostCmdType::ExposeChildren, xid, 0, 0});
}

extern "C" void x11_proto_bridge_screen_layout_changed(void)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  HostCmd c{};
  c.type = HostCmdType::ScreenLayoutChanged;
  c.xid = x11::kRootXid;
  srv->hostCmds().push(c);
}


static x11::XProtoDaemon g_daemon;
// Wire daemon pointer for bridge functions.
// (set in x11_proto_start_daemon, cleared in x11_proto_stop_daemon)

// Version string: single source of truth is SwiftX11Version.h
#include "SwiftX11Version.h"
#include "Utils/MachTime.hpp"
static constexpr const char* kSwiftX11Version = SWIFTX11_VERSION;

const char* swiftx11_version(void)
{
  return kSwiftX11Version;
}

const char* swiftx11_build_date(void)
{
  return __DATE__;
}

extern "C" int x11_proto_start_daemon(int display)
{
  fprintf(stderr, "\n========================================\n");
  fprintf(stderr, "  SwiftX11 v%s  (C++ protocol core)\n", kSwiftX11Version);
  fprintf(stderr, "  display=:%d\n", display);
  fprintf(stderr, "========================================\n\n");
  g_daemon_ptr = &g_daemon;
  // Legacy: enable both TCP (0.0.0.0) and Unix socket
  return g_daemon.start(display, true, true, "0.0.0.0") ? 1 : 0;
}

extern "C" int x11_proto_start_daemon_ex(int display, int enable_tcp, int enable_unix,
                                          const char* tcp_bind_addr)
{
  fprintf(stderr, "\n========================================\n");
  fprintf(stderr, "  SwiftX11 v%s  (C++ protocol core)\n", kSwiftX11Version);
  fprintf(stderr, "  display=:%d  tcp=%s unix=%s bind=%s\n",
          display,
          enable_tcp ? "on" : "off",
          enable_unix ? "on" : "off",
          tcp_bind_addr ? tcp_bind_addr : "0.0.0.0");
  fprintf(stderr, "========================================\n\n");
  g_daemon_ptr = &g_daemon;
  return g_daemon.start(display,
                        enable_tcp != 0,
                        enable_unix != 0,
                        tcp_bind_addr ? tcp_bind_addr : "0.0.0.0") ? 1 : 0;
}

extern "C" void x11_proto_stop_daemon(void)
{
  g_daemon.stop();
  g_daemon_ptr = nullptr;
}

// Legacy fallback for callers that only have DOWN/UP without a button number.
// If you don't want this, you can omit it and just ignore DOWN/UP here.
extern "C" void x11_proto_bridge_post_pointer_button_legacy(uint32_t xid,
                                                int is_press,
                                                int32_t x_px, int32_t y_px,
                                                uint32_t buttons, uint32_t modifiers);


// pointer/mouse handling
extern "C" {

//void x11_proto_bridge_post_pointer_move(uint32_t xid,
//                                       int32_t x_px, int32_t y_px,
//                                       uint32_t buttons, uint32_t modifiers)
//{
//
//  HostCmd c;
//  c.type = HostCmdType::PointerMove;
//  c.xid = xid;
//  c.x_px = x_px;
//  c.y_px = y_px;
//  c.buttonsMask = buttons;
//  c.modsMask = modifiers;
//  hostcmd_push(c);
//}

void x11_proto_bridge_post_pointer_button_legacy(uint32_t xid,
                                                int is_press,
                                                int32_t x_px, int32_t y_px,
                                                uint32_t buttons, uint32_t modifiers)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  HostCmd c;
  c.type = HostCmdType::Button;
  c.xid = xid;
  c.isDown = is_press ? 1 : 0;
  c.button = 0; // legacy / unknown button
  c.win_x_u = x_px;
  c.win_y_u = y_px;
  c.buttonsMask = buttons;
  c.modsMask = modifiers;
  srv->hostCmds().push(c);
}

} // extern "C"

extern "C" void x11_proto_bridge_post_pointer_move2(uint32_t xid,
                                                   int32_t win_x_u, int32_t win_y_u,
                                                   int32_t root_x_u, int32_t root_y_u,
                                                   uint8_t deliver,
                                                   uint32_t buttons,
                                                   uint32_t modifiers)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  HostCmd c;
  c.type = HostCmdType::PointerMove;
  c.xid = xid;
  c.win_x_u = win_x_u;
  c.win_y_u = win_y_u;
  c.root_x_u = root_x_u;
  c.root_y_u = root_y_u;
  c.deliver = deliver;
  c.buttonsMask = buttons;
  c.modsMask = modifiers;
  srv->hostCmds().push(c);
}


extern "C" void x11_proto_bridge_post_pointer_button(uint32_t xid,
                                                     uint8_t is_press,
                                                     uint8_t button,
                                                     int32_t win_x_u, int32_t win_y_u,
                                                     int32_t root_x_u, int32_t root_y_u,
                                                     uint32_t buttons,
                                                     uint32_t modifiers)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  HostCmd c;
  c.type = HostCmdType::Button;
  c.xid = xid;
  c.isDown = is_press ? 1 : 0;
  c.button = button;
  c.win_x_u = win_x_u;
  c.win_y_u = win_y_u;
  c.root_x_u = root_x_u;
  c.root_y_u = root_y_u;
  c.buttonsMask = buttons;
  c.modsMask = modifiers;
  srv->hostCmds().push(c);
}


extern "C" void x11_proto_bridge_post_scroll(uint32_t xid,
                                            uint8_t axis,
                                            int16_t ticks,
                                            int32_t win_x_u, int32_t win_y_u,
                                            uint32_t buttons,
                                            uint32_t modifiers)
{
  if (xid == 0) return;

  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  HostCmd c;
  c.type = HostCmdType::ScrollTicks;
  c.xid = xid;
  c.axis = axis;
  c.ticks = ticks;
  c.win_x_u = win_x_u;
  c.win_y_u = win_y_u;
  c.buttonsMask = buttons;
  c.modsMask = modifiers;
  srv->hostCmds().push(c);
}

extern "C" void x11_proto_bridge_post_key(uint32_t xid,
                                         uint8_t is_down,
                                         uint32_t keycode,
                                         uint32_t modifiers,
                                         uint8_t is_repeat)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  HostCmd c;
  c.type = HostCmdType::Key;
  c.xid = xid;               // may be 0 → route to focus on C++ side
  c.isDown = is_down ? 1 : 0;
  c.isRepeat = is_repeat ? 1 : 0;
  c.keyCode = keycode;
  c.modsMask = modifiers;
  srv->hostCmds().push(c);
}

extern "C" void x11_proto_bridge_post_enter(uint32_t xid,
                                           int32_t win_x_u, int32_t win_y_u,
                                           int32_t root_x_u, int32_t root_y_u,
                                           uint32_t modifiers)
{
  if (xid == 0) return;

  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  HostCmd c;
  c.type = HostCmdType::PointerEnter;
  c.xid = xid;
  c.win_x_u = win_x_u;
  c.win_y_u = win_y_u;
  c.root_x_u = root_x_u;
  c.root_y_u = root_y_u;
  c.modsMask = modifiers;
  srv->hostCmds().push(c);
}

extern "C" void x11_proto_bridge_post_leave(uint32_t xid,
                                           int32_t win_x_u, int32_t win_y_u,
                                           int32_t root_x_u, int32_t root_y_u,
                                           uint32_t modifiers)
{
  if (xid == 0) return;

  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  HostCmd c;
  c.type = HostCmdType::PointerLeave;
  c.xid = xid;
  c.win_x_u = win_x_u;
  c.win_y_u = win_y_u;
  c.root_x_u = root_x_u;
  c.root_y_u = root_y_u;
  c.modsMask = modifiers;
  srv->hostCmds().push(c);
}

extern "C" void x11_proto_bridge_post_focus(uint32_t xid,
                                           uint8_t focused)
{
  if (xid == 0) return;

  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  HostCmd c;
  c.type = HostCmdType::Focus;
  c.xid = xid;
  c.focused = focused ? 1 : 0;
  srv->hostCmds().push(c);
}


extern "C" uint32_t x11_cpp_list_descendants(uint32_t host, uint32_t* out, uint32_t cap)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return 0;
  if (!out || cap == 0) return 0;
  if (host == 0) return 0;

  std::vector<uint32_t> kids = srv->ctx().windows().descendantsOf(host);

  const uint32_t n = (uint32_t)std::min<size_t>(kids.size(), cap);
  for (uint32_t i = 0; i < n; i++) out[i] = kids[i];
  return n;
}

extern "C" int x11_cpp_get_window_geom(uint32_t xid,
                                      uint32_t* out_parent,
                                      int16_t* out_x,
                                      int16_t* out_y,
                                      uint16_t* out_w,
                                      uint16_t* out_h,
                                      int* out_mapped)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return 0;
  if (xid == 0) return 0;

  x11::WindowView vw{};
  if (!srv->ctx().windows().snapshot(xid, vw)) return 0;

  if (out_parent) *out_parent = vw.parent_xid;
  if (out_x)      *out_x      = vw.x;
  if (out_y)      *out_y      = vw.y;
  if (out_w)      *out_w      = vw.w;
  if (out_h)      *out_h      = vw.h;
  if (out_mapped) *out_mapped = vw.mapped ? 1 : 0;

  return 1;
}

extern "C" int x11_cpp_get_abs_pos_in_host(uint32_t host, uint32_t xid,
                                          int32_t* out_abs_x,
                                          int32_t* out_abs_y)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return 0;
  if (!out_abs_x || !out_abs_y) return 0;
  if (host == 0 || xid == 0) return 0;

  // Walk from xid up to host, accumulating relative x/y.
  int32_t ax = 0;
  int32_t ay = 0;
  uint32_t cur = xid;

  // Safety to avoid infinite loops if parent pointers get weird.
  for (int hop = 0; hop < 256; hop++) {
    x11::WindowView vw{};
    if (!srv->ctx().windows().snapshot(cur, vw)) return 0;

    // Add this node's offset in its parent.
    ax += (int32_t)vw.x;
    ay += (int32_t)vw.y;

    if (cur == host) {
      // We included host's own x/y above; in typical X11, host x/y is relative to root.
      // For "position within host", we should NOT include host's offset.
      // So subtract host.x/host.y back out:
      ax -= (int32_t)vw.x;
      ay -= (int32_t)vw.y;

      *out_abs_x = ax;
      *out_abs_y = ay;
      return 1;
    }

    // Stop if we hit root-ish without reaching host.
    if (vw.parent_xid == 0 || vw.parent_xid == 1) return 0;

    cur = vw.parent_xid;
  }

  return 0;
}


extern "C" int x11_cpp_copy_host_surface_bgra(uint32_t xid,
                                             uint8_t* out_bytes,
                                             int32_t out_cap,
                                             int32_t* out_w,
                                             int32_t* out_h,
                                             int32_t* out_bpr)
{
  if (out_w)   *out_w = 0;
  if (out_h)   *out_h = 0;
  if (out_bpr) *out_bpr = 0;

  auto* srv = x11_proto_bridge_get_server();
  if (!srv || xid == 0) return 0;

  auto& ctx = srv->ctx();

  // Present is host-owned.
  uint32_t host = xid;
  if (ctx.windows().exists(xid)) {
    const uint32_t h = ctx.windows().topLevelAncestorOf(xid);
    if (h) host = h;
  }

  x11::SurfaceDesc s{};
  if (!ctx.surfaces().get(host, s) || !s.ptr || s.bytesPerRow == 0 || s.w == 0 || s.h == 0) {
#if X11_TRACE_PRESENT_ENABLED
    TS_FPRINTF("[COPY_SURFACE] xid=0x%08X host=0x%08X FAIL no surface (ptr=%p wh=%ux%u bpr=%u)\n",
            (unsigned)xid, (unsigned)host, s.ptr, (unsigned)s.w, (unsigned)s.h, (unsigned)s.bytesPerRow);
#endif
    return 0;
  }

  const int32_t w   = (int32_t)s.w;
  const int32_t hgt = (int32_t)s.h;

  // ABI: we return tightly packed BGRA rows (matches old x11_xproto_copy_window_bgra behavior)
  const int32_t tightBpr = w * 4;

  // sanity: source stride must be at least tight row bytes
  if ((int32_t)s.bytesPerRow < tightBpr) return 0;

  const int64_t needed64 = (int64_t)tightBpr * (int64_t)hgt;
  if (needed64 <= 0 || needed64 > INT32_MAX) return 0;
  const int32_t needed = (int32_t)needed64;

  if (out_w)   *out_w   = w;
  if (out_h)   *out_h   = hgt;
  if (out_bpr) *out_bpr = tightBpr;

  // size-only query
  if (!out_bytes || out_cap == 0) return 1;
  if (out_cap < needed) return 0;

  const uint8_t* src = (const uint8_t*)s.ptr;
  const uint32_t srcBpr = s.bytesPerRow;

  // Pack row-by-row from padded surface into tight output
  for (int32_t y = 0; y < hgt; y++) {
    const uint8_t* srow = src + (size_t)y * (size_t)srcBpr;
    uint8_t*       drow = out_bytes + (size_t)y * (size_t)tightBpr;
    std::memcpy(drow, srow, (size_t)tightBpr);
  }

#ifdef X11_TRACE_VERBOSE
  // Sample a few pixels from the source surface for debugging.
  {
    const uint32_t* px = (const uint32_t*)s.ptr;
    const uint32_t mid = (uint32_t)((size_t)(hgt/2) * (size_t)(srcBpr/4) + (size_t)(w/2));
    const uint32_t p0 = px[0];
    const uint32_t pm = (mid < (uint32_t)((size_t)hgt * (size_t)(srcBpr/4))) ? px[mid] : 0;
    // Count non-white pixels in first row.
    uint32_t nonwhite = 0;
    for (int32_t x = 0; x < w; x++) {
      if ((px[x] & 0x00FFFFFFu) != 0x00FFFFFFu) nonwhite++;
    }
    TS_FPRINTF("[COPY_SURFACE] xid=0x%08X host=0x%08X wh=%dx%d bpr=%d "
            "p0=0x%08X pmid=0x%08X row0_nonwhite=%u\n",
            (unsigned)xid, (unsigned)host, (int)w, (int)hgt, (int)tightBpr,
            (unsigned)p0, (unsigned)pm, (unsigned)nonwhite);
  }
#endif

  return 1;
}
