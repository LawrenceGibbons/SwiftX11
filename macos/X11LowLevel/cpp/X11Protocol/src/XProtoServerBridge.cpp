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
    fprintf(stderr, "[BG_FILL_RETRY] wid=0x%08X pixmap=0x%08X wh=%ux%u stride=%u\n",
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
    fprintf(stderr, "[BG_FILL_RETRY] wid=0x%08X SKIP resolve failed\n", (unsigned)wid);
#endif
    return;
  }
  if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) return;

#if X11_TRACE_PRESENT_ENABLED
  fprintf(stderr, "[BG_FILL_RETRY] wid=0x%08X bg=0x%08X wh=%ux%u stride=%u off=(%d,%d)\n",
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
  fprintf(stderr, "[BORDER_RETRY] childXid=0x%08X parent=0x%08X bw=%d bp=0x%08X at=(%d,%d) total=%dx%d\n",
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
    fprintf(stderr, "[EXPOSE_SEND] wid=0x%08X SKIP (no WindowView)\n", (unsigned)wid);
#endif
    return;
  }

#if X11_TRACE_LIFECYCLE_ENABLED
  fprintf(stderr, "[EXPOSE_SEND] wid=0x%08X wh=%ux%u mapped=%d evmask=0x%08X count=%u\n",
          (unsigned)wid, (unsigned)wv->w, (unsigned)wv->h,
          (int)wv->mapped, (unsigned)wv->event_mask, (unsigned)count);
#endif

  auto ev = x11::wireev::buildExpose(ctx.transport().lastSeq(),
                                   wid,
                                   0, 0,
                                   wv->w, wv->h,
                                   count);
  bool sent = ctx.transport().sendEvent32(wid, ev.data());

#ifndef NDEBUG
  // Check if this window (or its host) is OR — helps diagnose blank popup text.
  {
    bool isOR = wv->override_redirect;
    if (!isOR && wv->parent_xid != 0 && wv->parent_xid != 1) {
      uint32_t host = ctx.windows().topLevelAncestorOf(wid);
      x11::WindowView hv{};
      if (host && ctx.windows().snapshot(host, hv)) isOR = hv.override_redirect;
    }
    if (isOR) {
      fprintf(stderr, "[OR_EXPOSE] Expose wid=0x%08X wh=%ux%u sent=%d "
              "owner_fd=%d client_fd=%d evmask=0x%08X\n",
              (unsigned)wid,
              (unsigned)wv->w, (unsigned)wv->h,
              (int)sent, wv->owner_fd, ctx.transport().clientFd(),
              (unsigned)wv->event_mask);
    }
  }
#endif
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
  // Check if host is override-redirect (for diagnostic traces)
  bool hostIsOR = false;
  {
    x11::WindowView hv{};
    if (ctx.windows().snapshot(hostXid, hv)) hostIsOR = hv.override_redirect;
  }

#ifndef NDEBUG
  if (hostIsOR) {
    x11::SurfaceDesc dbgS{};
    bool hasSurf = ctx.surfaces().get(hostXid, dbgS);
    fprintf(stderr, "[OR_EXPOSE_SUBTREE] host=0x%08X hasSurface=%d surfWH=%ux%u bpr=%u ptr=%p client_fd=%d\n",
            (unsigned)hostXid, (int)hasSurf,
            (unsigned)dbgS.w, (unsigned)dbgS.h,
            (unsigned)dbgS.bytesPerRow, dbgS.ptr,
            ctx.transport().clientFd());
  }
#endif
#if X11_TRACE_LIFECYCLE_ENABLED
  fprintf(stderr, "[EXPOSE_SUBTREE] host=0x%08X\n", (unsigned)hostXid);
#endif

  // Fill background + Expose the host itself.
  fillWindowBackgroundIfReady(ctx, hostXid);

  // Collect mapped descendants and fill their borders + backgrounds first
  // (X11 spec: server paints backgrounds before delivering Expose).
  auto kids = ctx.windows().descendantsOf(hostXid);

#ifndef NDEBUG
  if (hostIsOR) {
    fprintf(stderr, "[OR_EXPOSE_SUBTREE] host=0x%08X total_descendants=%zu\n",
            (unsigned)hostXid, kids.size());
  }
#endif
#if X11_TRACE_LIFECYCLE_ENABLED
  fprintf(stderr, "[EXPOSE_SUBTREE] host=0x%08X descendants=%zu\n",
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

#ifndef NDEBUG
    if (hostIsOR) {
      x11::DrawableRW dbgDst{};
      bool resolved = x11::resolveDrawableRW(ctx, kid, dbgDst);
      fprintf(stderr, "[OR_EXPOSE_SUBTREE] kid=0x%08X mapped=%d wh=%ux%u pos=(%d,%d) owner_fd=%d resolved=%d res_wh=%ux%u off=(%d,%d)\n",
              (unsigned)kid, (int)kv.mapped,
              (unsigned)kv.w, (unsigned)kv.h,
              (int)kv.x, (int)kv.y,
              kv.owner_fd,
              (int)resolved,
              resolved ? (unsigned)dbgDst.w : 0u,
              resolved ? (unsigned)dbgDst.h : 0u,
              resolved ? (int)dbgDst.offsetX : 0,
              resolved ? (int)dbgDst.offsetY : 0);
    }
#endif
#if X11_TRACE_LIFECYCLE_ENABLED
    {
      x11::DrawableRW dbgDst{};
      bool resolved = x11::resolveDrawableRW(ctx, kid, dbgDst);
      fprintf(stderr, "[EXPOSE_SUBTREE] kid=0x%08X resolved=%d wh=%ux%u off=(%d,%d) stride=%u\n",
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

#ifndef NDEBUG
  if (hostIsOR) {
    fprintf(stderr, "[OR_EXPOSE_SUBTREE] host=0x%08X sending Expose to %zu mapped kids + host\n",
            (unsigned)hostXid, mappedKids.size());
  }
#endif

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
          fprintf(stderr, "[SET_PRESENTABLE] xid=0x%08X\n", (unsigned)c.xid);
          {
            x11::SurfaceDesc dbgS{};
            bool hasSurf = ctx.surfaces().get(c.xid, dbgS);
            fprintf(stderr, "[SET_PRESENTABLE] xid=0x%08X hasSurface=%d surfWH=%ux%u bpr=%u ptr=%p\n",
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

          if (haveSV && !sv.surface_resize_exposed) {
            // Case A: initial surface growth — full re-expose needed.
            // The first sendExposeSubtree (at SetPresentable time) may have
            // run when the surface was at its initial (small) size.  Children
            // at far offsets were clipped to zero and never rendered.
            // Uses surface_resize_exposed (not presentable) because
            // SetPresentable may have already set presentable=true while the
            // surface was still small.
#if X11_TRACE_RESIZE_ENABLED
            fprintf(stderr, "[SURFACE_RESIZED] xid=0x%08X (initial) -> re-expose subtree\n",
                    (unsigned)c.xid);
#endif
            sendExposeSubtree(ctx, srv->eventOps(), c.xid);

            // Write full-window damage for the initial expose.
            x11_shared_damage_union(c.xid, 0, 0, (int32_t)sv.w, (int32_t)sv.h);
            x11_ui_push_damage(c.xid, 0, 0, (int32_t)sv.w, (int32_t)sv.h);
            ctx.windows().markDirty(c.xid);
            ctx.windows().setSurfaceResizeExposed(c.xid, true);
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
            fprintf(stderr, "[SURFACE_RESIZED] xid=0x%08X (resize) -> skip\n",
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

          // Find what we're entering *within* this host, using existing host-local coords.
          uint32_t under = x11::pickDeepestMappedWindowAtHostPoint(ctx, host,
                                                                  ctx.input().win_x_u,
                                                                  ctx.input().win_y_u);
          if (!under) under = host;

          // Pointer ownership should be the window under the pointer, not the host.
          ctx.input().enter(under);

          // Apply cursor to the Cocoa host window, choosing cursor from the routed pointer target.
          const uint32_t cursorTarget = ctx.input().routePointer(under);
          maybeApplyCursor(ctx, host, cursorTarget);

          srv->eventOps().sendCrossingEvent(ctx, under, /*is_enter=*/true,
                                            ctx.input().root_x_u, ctx.input().root_y_u,
                                            ctx.input().buttons, c.modsMask);
          break;
        }


        // ------------------- PointerLeave
        case HostCmdType::PointerLeave: {
          const uint32_t host = c.xid ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // Window that currently "has" the pointer (drag wins).
          uint32_t leaveWin = 0;
          if (ctx.input().drag_xid)       leaveWin = ctx.input().drag_xid;
          else if (ctx.input().pointer_xid) leaveWin = ctx.input().pointer_xid;
          else                             leaveWin = host;

          // Update pointer ownership state.
          ctx.input().leave(leaveWin);

          // After leaving, cursor should usually fall back (focus/host/inherit).
          const uint32_t cursorTarget = ctx.input().routePointer(host);
          maybeApplyCursor(ctx, host, cursorTarget);

          srv->eventOps().sendCrossingEvent(ctx, leaveWin, /*is_enter=*/false,
                                            ctx.input().root_x_u, ctx.input().root_y_u,
                                            ctx.input().buttons, c.modsMask);
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
            ctx.input().focus_host = host;

            // Focus the HOST — let the toolkit propagate to children
            // via SetInputFocus (opcode 42).
            ctx.input().focus_xid = host;
            if (ctx.input().drag_xid == 0) ctx.input().pointer_xid = host;

            // FocusOut to previous focus window (if different)
            if (oldFocus && oldFocus != host) {
              srv->eventOps().sendFocusEventDirect(ctx, oldFocus, /*is_in=*/false);
            }
            // FocusIn to HOST — always delivered (bypass mask check)
            srv->eventOps().sendFocusEventDirect(ctx, host, /*is_in=*/true);

          } else {
            // Losing focus on this host — only act if this host actually
            // had focus.  A stale FocusOut for a destroyed/non-focused window
            // must not steal focus from the real focus holder.
            if (ctx.input().focus_host == host) {
              ctx.input().focus_host = 0;

              if (oldFocus) {
                srv->eventOps().sendFocusEventDirect(ctx, oldFocus, /*is_in=*/false);
              }

              ctx.input().focus_xid = 0;
              if (ctx.input().drag_xid == 0) ctx.input().pointer_xid = 0;
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
          // pointer is actually over a higher-stacking window (popup menu),
          // correct the host using root coords from the last motion event.
          uint32_t effectiveHost = host;
          int32_t effectiveWinX = c.win_x_u;
          int32_t effectiveWinY = c.win_y_u;
          bool hostCorrected = false;
          {
            const int32_t rx = ctx.input().root_x_u;
            const int32_t ry = ctx.input().root_y_u;
            auto topLevels = ctx.windows().childrenInStackOrder(1);
            for (auto it = topLevels.rbegin(); it != topLevels.rend(); ++it) {
              if (*it == host) break;
              x11::WindowView vw{};
              if (!ctx.windows().snapshot(*it, vw)) continue;
              if (!vw.mapped) continue;
              int32_t bw = (int32_t)vw.border_width;
              if (rx >= vw.x - bw && rx < vw.x + (int32_t)vw.w + bw &&
                  ry >= vw.y - bw && ry < vw.y + (int32_t)vw.h + bw) {
                effectiveHost = *it;
                effectiveWinX = rx - vw.x;
                effectiveWinY = ry - vw.y;
                hostCorrected = true;
                break;
              }
            }
          }

          // ---- Check for active pointer grab (GrabPointer) ----
          x11::PointerGrab activeGrab{};
          const bool haveActiveGrab = ctx.grabs().getPointerGrab(activeGrab) && activeGrab.active;

          // ---- STEP 1: Pick the deepest window BEFORE updating drag state ----
          // This is critical: InputState::button() sets drag_xid on the
          // 0→nonzero button transition. We must pick the child under the
          // pointer first, so drag_xid gets set to the correct child window
          // (not the host).
          uint32_t under = 0;

          if (haveActiveGrab && !activeGrab.ownerEvents) {
            // Active GrabPointer with owner_events=False:
            // All button events go to the grab window, filtered by grab eventMask.
            under = activeGrab.grabWindow;
          } else if (ctx.input().drag_xid && !hostCorrected) {
            // Already in an active grab/drag, pointer still over same host
            under = ctx.input().drag_xid;
          } else {
            under = x11::pickDeepestMappedWindowAtHostPoint(ctx, effectiveHost,
                                                            effectiveWinX,
                                                            effectiveWinY);
            if (!under) under = effectiveHost;

            // ---- STEP 2: Check passive grabs (GrabButton) on press ----
            // Walk from the deepest window up to the host checking each
            // ancestor for a matching passive grab. If found, the grab
            // window becomes the button event target and the implicit
            // pointer grab owner (via drag_xid).
            if (c.isDown && !haveActiveGrab) {
              x11::PassiveGrab pg{};
              uint32_t checkWin = under;
              bool foundGrab = false;
              int safety = 0;
              // GrabButton stores modifiers in X11 wire format (ControlMask=bit2).
              // c.modsMask uses internal format (Ctrl=bit1). Convert to X11.
              const uint16_t x11Mods = x11::input::toX11State(0, c.modsMask) & 0xFF;
              while (checkWin && safety++ < 64) {
                if (ctx.grabs().match(checkWin, c.button, x11Mods, pg)) {
                  foundGrab = true;
                  under = pg.grabWindow;
                  break;
                }
                if (checkWin == host) break;
                x11::WindowView vw{};
                if (!ctx.windows().snapshot(checkWin, vw)) break;
                checkWin = vw.parent_xid;
              }
            #ifndef NDEBUG
              fprintf(stderr,
                      "[BTN_GRAB] under=0x%08X btn=%u x11Mods=0x%04X grab=%s grabWin=0x%08X\n",
                      (unsigned)under, (unsigned)c.button, (unsigned)x11Mods,
                      foundGrab ? "YES" : "no",
                      foundGrab ? (unsigned)pg.grabWindow : 0u);
            #endif
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

          // NOTE: No click-to-focus here. Focus is handled by the
          // HostCmdType::Focus handler (Cocoa becomeKey/resignKey) which
          // sends FocusIn to the HOST. The toolkit (Xt) then propagates
          // to children via SetInputFocus.

          // Pick a delivery window that selected the relevant mask.
          auto wantsBtn = [&](uint32_t xid) -> bool {
            if (!xid) return false;
            const x11::WindowView* vw = ctx.window(xid);
            if (!vw || vw->owner_fd <= 0) return false;
            const uint32_t mask = vw->event_mask;
            const bool wantPress   = (mask & x11::mask::ButtonPress) != 0;
            const bool wantRelease = (mask & x11::mask::ButtonRelease) != 0;
            return (c.isDown ? wantPress : wantRelease);
          };

          uint32_t deliver = under;

          // If under doesn't select, climb to parent until effective host (simple propagation).
          if (!wantsBtn(deliver)) {
            uint32_t cur = under;
            int safety = 0;
            while (cur && cur != effectiveHost) {
              x11::WindowView vw{};
              if (!ctx.windows().snapshot(cur, vw)) break;
              cur = vw.parent_xid;
              if (wantsBtn(cur)) { deliver = cur; break; }
              if (++safety > 64) break;
            }
            // If still not found, try effective host last.
            if (!wantsBtn(deliver) && wantsBtn(effectiveHost)) deliver = effectiveHost;
          }

          // If nobody wants it, drop.
          if (!wantsBtn(deliver)) break;

          // child field: if delivering to ancestor, child is the subwindow under pointer
          const uint32_t child = (deliver != under) ? under : 0;

        #ifndef NDEBUG
          {
            // Enhanced button event trace for debugging xcalc button routing
            fprintf(stderr,
                    "[BTN] host=0x%08X under=0x%08X deliver=0x%08X child=0x%08X down=%d btn=%u drag=0x%08X\n",
                    (unsigned)host, (unsigned)under, (unsigned)deliver, (unsigned)child,
                    (int)(c.isDown != 0), (unsigned)c.button, (unsigned)ctx.input().drag_xid);
            fprintf(stderr,
                    "[BTN]   win_xy=(%d,%d) root_xy=(%d,%d)\n",
                    (int)ctx.input().win_x_u, (int)ctx.input().win_y_u,
                    (int)ctx.input().root_x_u, (int)ctx.input().root_y_u);

            // Print geometry of 'under' and 'deliver' windows
            x11::WindowView underVw{}, deliverVw{};
            if (ctx.windows().snapshot(under, underVw)) {
              fprintf(stderr,
                      "[BTN]   under geom=(%d,%d %ux%u) parent=0x%08X mask=0x%08X\n",
                      (int)underVw.x, (int)underVw.y,
                      (unsigned)underVw.w, (unsigned)underVw.h,
                      (unsigned)underVw.parent_xid, (unsigned)underVw.event_mask);
            }
            if (deliver != under && ctx.windows().snapshot(deliver, deliverVw)) {
              fprintf(stderr,
                      "[BTN]   deliver geom=(%d,%d %ux%u) parent=0x%08X mask=0x%08X\n",
                      (int)deliverVw.x, (int)deliverVw.y,
                      (unsigned)deliverVw.w, (unsigned)deliverVw.h,
                      (unsigned)deliverVw.parent_xid, (unsigned)deliverVw.event_mask);
            }

            // On press, dump all immediate children of deliver's parent (sibling buttons)
            if (c.isDown) {
              uint32_t parentOfDeliver = 0;
              x11::WindowView dvw{};
              if (ctx.windows().snapshot(deliver, dvw)) parentOfDeliver = dvw.parent_xid;
              if (parentOfDeliver && parentOfDeliver != host) {
                // Dump siblings (other buttons)
                auto siblings = ctx.windows().descendantsOf(parentOfDeliver);
                int printed = 0;
                for (uint32_t sib : siblings) {
                  x11::WindowView sv{};
                  if (!ctx.windows().snapshot(sib, sv)) continue;
                  if (sv.parent_xid != parentOfDeliver) continue; // only direct children
                  if (!sv.mapped) continue;
                  fprintf(stderr,
                          "[BTN]   sibling=0x%08X geom=(%d,%d %ux%u) mask=0x%08X\n",
                          (unsigned)sib, (int)sv.x, (int)sv.y,
                          (unsigned)sv.w, (unsigned)sv.h, (unsigned)sv.event_mask);
                  if (++printed > 40) { fprintf(stderr, "[BTN]   ... (truncated)\n"); break; }
                }
              }
            }
          }
        #endif

          srv->eventOps().sendButtonEvent(ctx, deliver,
                                          c.isDown != 0, c.button,
                                          ctx.input().root_x_u, ctx.input().root_y_u,
                                          buttonsBefore, c.modsMask,
                                          child);
          break;
        }


        // ------------------- ScrollTicks
        case HostCmdType::ScrollTicks: {
          const uint32_t host = c.xid ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // bring-up: ignore horizontal (axis==1) if desired
          if (c.axis == 1) break;

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

          for (int i = 0; i < nClamped; i++) {
            const uint8_t btn = wheelButton(c.axis, (int16_t)dir);

        #ifdef X11_TRACE_VERBOSE
            fprintf(stderr,
                    "[SCROLL] host=0x%08X target=0x%08X axis=%u ticks=%d btn=%u win=(%d,%d) root=(%d,%d) t=%u\n",
                    (unsigned)host, (unsigned)target,
                    (unsigned)c.axis, (int)ticks, (unsigned)btn,
                    (int)c.win_x_u, (int)c.win_y_u,
                    (int)rx, (int)ry,
                    (unsigned)x11_now_ms_monotonic());
        #endif

            srv->eventOps().sendButtonEvent(ctx, target,
                                            /*is_press=*/true, btn,
                                            rx, ry,
                                            ctx.input().buttons, c.modsMask,
                                            /*child_xid=*/0);

            const uint32_t wheelMask = (btn >= 1 && btn <= 31) ? (1u << (btn - 1u)) : 0;
            srv->eventOps().sendButtonEvent(ctx, target,
                                            /*is_press=*/false, btn,
                                            rx, ry,
                                            (ctx.input().buttons | wheelMask), c.modsMask,
                                            /*child_xid=*/0);
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

          // Keep canonical mods in sync
          ctx.input().mods = c.modsMask;

          auto wantsKey = [&](uint32_t xid) -> bool {
            if (!xid) return false;
            const x11::WindowView* vw = ctx.window(xid);
            if (!vw || vw->owner_fd <= 0) return false;
            const uint32_t mask = vw->event_mask;
            const bool wantPress   = (mask & x11::mask::KeyPress) != 0;
            const bool wantRelease = (mask & x11::mask::KeyRelease) != 0;
            return c.isDown ? wantPress : wantRelease;
          };

          // Primary: keyboard focus (only if it belongs to this host)
          uint32_t target = 0;
          const uint32_t focus = ctx.input().focus_xid;
          if (focus != 0) {
            const uint32_t focusHost = ctx.windows().topLevelAncestorOf(focus);
            if (focusHost == host) target = focus;
          }
          if (!target) target = host; // fallback: host

          // If target doesn't select, climb parent chain until host (simple propagation).
          if (!wantsKey(target)) {
            uint32_t cur = target;
            int safety = 0;
            while (cur && cur != host) {
              x11::WindowView vw{};
              if (!ctx.windows().snapshot(cur, vw)) break;
              cur = vw.parent_xid;
              if (wantsKey(cur)) { target = cur; break; }
              if (++safety > 64) break;
            }
            if (!wantsKey(target) && wantsKey(host)) target = host;
            if (!wantsKey(target)) break; // nobody wants it
          }

        #ifdef X11_TRACE_VERBOSE
          fprintf(stderr,
                  "[KEY] host=0x%08X focus=0x%08X deliver=0x%08X down=%d kc=%u mods=0x%X\n",
                  (unsigned)host,
                  (unsigned)focus,
                  (unsigned)target,
                  (int)(c.isDown != 0),
                  (unsigned)x11_kc,
                  (unsigned)c.modsMask);
        #endif

          srv->eventOps().sendKeyEvent(ctx, target,
                                       c.isDown != 0,
                                       x11_kc,
                                       ctx.input().buttons, c.modsMask);
          break;
        }

        // ------------------- ClipboardCapture
        // Proactive selection request: when an X11 app claims PRIMARY or
        // CLIPBOARD ownership, we send it a SelectionRequest so it writes
        // its data to a property we can capture and push to macOS pasteboard.
        // This runs on a separate poll iteration via drainHostCommands with
        // the owning client properly activated — avoiding the fatal IO error
        // that occurred when sending during the same readAndDispatch call.
        case HostCmdType::ClipboardCapture: {
          const uint32_t owner     = c.xid;
          const uint32_t selection = c.keyCode; // selection atom stored in keyCode
          if (!owner) break;

          uint8_t ev[32] = {0};
          ev[0] = 30; // SelectionRequest
          x11::wire::wr16_le(ev + 2, ctx.transport().lastSeq()); // sequence number (required by XCB)
          x11::wire::wr32_le(ev + 4,  0); // time = CurrentTime
          x11::wire::wr32_le(ev + 8,  owner);                    // owner
          x11::wire::wr32_le(ev + 12, owner);                    // requestor = owner
          x11::wire::wr32_le(ev + 16, selection);
          x11::wire::wr32_le(ev + 20, x11::atom::kUTF8_STRING); // target
          x11::wire::wr32_le(ev + 24, x11::atom::kSWIFTX11_CLIP); // property
          (void)ctx.transport().sendEvent32(owner, ev);

#ifndef NDEBUG
          fprintf(stderr, "[CLIPBOARD] HostCmd SelectionRequest sel=%u owner=0x%08X\n",
                  (unsigned)selection, (unsigned)owner);
#endif
          break;
        }

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

extern "C" void x11_proto_bridge_surface_resized(uint32_t xid)
{
  if (xid == 0) return;
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  srv->hostCmds().push(HostCmd{HostCmdType::SurfaceResized, xid, 0, 0});
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
static constexpr const char* kSwiftX11Version = SWIFTX11_VERSION;

const char* swiftx11_version(void)
{
  return kSwiftX11Version;
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
                                         uint32_t modifiers)
{
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  HostCmd c;
  c.type = HostCmdType::Key;
  c.xid = xid;               // may be 0 → route to focus on C++ side
  c.isDown = is_down ? 1 : 0;
  c.keyCode = keycode;
  c.modsMask = modifiers;
  srv->hostCmds().push(c);
}

extern "C" void x11_proto_bridge_post_enter(uint32_t xid,
                                           int32_t win_x_u, int32_t win_y_u,
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
  c.modsMask = modifiers;
  srv->hostCmds().push(c);
}

extern "C" void x11_proto_bridge_post_leave(uint32_t xid,
                                           int32_t win_x_u, int32_t win_y_u,
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
    fprintf(stderr, "[COPY_SURFACE] xid=0x%08X host=0x%08X FAIL no surface (ptr=%p wh=%ux%u bpr=%u)\n",
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
    fprintf(stderr, "[COPY_SURFACE] xid=0x%08X host=0x%08X wh=%dx%d bpr=%d "
            "p0=0x%08X pmid=0x%08X row0_nonwhite=%u\n",
            (unsigned)xid, (unsigned)host, (int)w, (int)hgt, (int)tightBpr,
            (unsigned)p0, (unsigned)pm, (unsigned)nonwhite);
  }
#endif

  return 1;
}
