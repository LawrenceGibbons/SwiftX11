//  WindowOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#include "Ops/WindowOps.hpp"

#include "Core/XProtoContext.hpp"
#include "Utils/ByteReader.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Core/WindowTable.hpp"
#include "x11_requests.h"
#include "Core/HostResize.hpp"
#include "Core/X11CoreOpcodes.hpp"

// bridge
extern "C" {
  #include "x11_requests.h"
}

// util
#include "Damage.hpp"

// bridge
#include "XProtoServerBridge.h"
#include "x11_backend_fb.h"
#include <cstdio>   // snprintf
#include "Debug/x11_backend_fb_dbg.hpp"
extern "C" {
#include "SwiftX11Bridge.h"
}

namespace x11 {

  
WindowOps::WindowOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::CreateWindow,    &WindowOps::onMajor, this);  // CreateWindow
  reg.registerMajor(x11::opcode::DestroyWindow,   &WindowOps::onMajor, this);  // DestroyWindow
  reg.registerMajor(x11::opcode::MapWindow,       &WindowOps::onMajor, this);  // MapWindow
  reg.registerMajor(x11::opcode::MapSubwindows,   &WindowOps::onMajor, this);  // MapSubwindows
  reg.registerMajor(x11::opcode::UnmapWindow  ,   &WindowOps::onMajor, this);  // UnmapWindow
  reg.registerMajor(x11::opcode::UnmapSubwindows, &WindowOps::onMajor, this);  // UnmapSubwindows
}

void WindowOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<WindowOps*>(user)->handle(ctx, dc);
}

void WindowOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::CreateWindow    :  handleCreateWindow(ctx, dc.seq, dc.minor /*depth*/, dc.br); return;
    case x11::opcode::DestroyWindow   :  handleDestroyWindow(ctx, dc.seq, dc.br); return;
    case x11::opcode::MapWindow       :  handleMapWindow(ctx, dc.seq, dc.br); return;
    case x11::opcode::MapSubwindows   :  handleMapSubwindows(ctx, dc.seq, dc.br); return;
    case x11::opcode::UnmapWindow     : handleUnmapWindow(ctx, dc.seq, dc.br); return;
    case x11::opcode::UnmapSubwindows : handleUnmapSubwindows(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[WindowOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// ---- STUBS ----
// For now, just consume the request body so clients keep going.
// We’ll port each handler one-by-one from x11_xproto.c, keeping C authoritative
// until we’re ready to move window state into C++.

void WindowOps::handleCreateWindow(XProtoContext& ctx, uint16_t /*seq*/, uint8_t /*depth*/, ByteReader& br)
{
  // Matches C: remain < 28 guard (but here br is already the "remain" region)
  if (br.remaining() < 28) { br.skip(br.remaining()); return; }

  const uint32_t wid    = br.readU32();
  const uint32_t parent = br.readU32();
  const int16_t  x      = (int16_t)br.readU16();
  const int16_t  y      = (int16_t)br.readU16();
  uint16_t wpx          = br.readU16();
  uint16_t hpx          = br.readU16();

  (void)br.readU16(); // borderWidth
  (void)br.readU16(); // class
  (void)br.readU32(); // visual
  const uint32_t vmask = br.readU32();

  // Parse value-list for CWEventMask (bit 11), same as your C code.
  uint32_t event_mask = 0;
  if (vmask & (1u << 11)) {
    for (uint32_t bit = 0; bit < 32; bit++) {
      if (!(vmask & (1u << bit))) continue;
      if (br.remaining() < 4) break;
      const uint32_t val = br.readU32();
      if (bit == 11) event_mask = val;
    }
  } else {
    // Still must consume any remaining value list/padding
    // (Some clients include extra padding; safest is to just skip remaining.)
  }

  // Consume any extra trailing bytes/padding
  br.skip(br.remaining());

  // Update authoritative WindowTable (C++ side)
  const int owner_fd = ctx.transport().clientFd();
  ctx.windows().upsert(wid, parent, x, y, (wpx ? wpx : 1), (hpx ? hpx : 1), event_mask, owner_fd);
  ctx.windows().setMapped(wid, false);
  ctx.windows().setPresentable(wid, false);

  // Allocate/refresh C-side slot+FB
  int dirty = 0;
  if (!x11_backend_fb_create_slot(wid, wpx, hpx, owner_fd, &dirty)) {
    ctx.tracef("[WindowOps] CreateWindow failed wid=0x%08X\n", (unsigned)wid);
    return;
  }
  // FB exists now; record the callsite that *actually* established it.
  ctx.windows().noteFbResizeDbg(wid, "CreateWindow", __FILE__, __LINE__);
  
  if (dirty) ctx.windows().markDirty(wid);

  // Enqueue Swift window creation (same behavior as enqueue_create_window)
  // NOTE: If enqueue_create_window is C-static, call the request queue directly.
  // This matches your existing enqueue_create_window() implementation.
  char title[64];
  snprintf(title, sizeof(title), "xid=0x%08X", (unsigned)wid);
  x11_requests_push_create(wid, parent, title, (int32_t)(wpx ? wpx : 1), (int32_t)(hpx ? hpx : 1));
}
  
  
void WindowOps::handleDestroyWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  // Request body: CARD32 window
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());

  if (wid == 0) return;

  // 1) Authoritative C++ state
  ctx.windows().erase(wid);

  // 2) C-side cleanup of legacy state (framebuffers, props, g_wins) — transitional
  // This should free the legacy fb/pixmaps/props that are still owned by x11_xproto.c
  x11_backend_fb_destroy(wid);

  // 3) Swift/UI teardown event path (existing behavior)
  // This queues X11_REQ_DESTROY -> shim -> Swift close
  x11_requests_push_destroy(wid);
}
  
  
  void WindowOps::handleMapWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t wid = br.readU32();
    br.skip(br.remaining());
    if (wid == 0) return;

    // 1) Update authoritative table
    ctx.windows().setMapped(wid, true);

    // Rootless rule: only top-level host windows drive Cocoa.
    const uint32_t host = ctx.windows().topLevelAncestorOf(wid);

    // 2) Swift-side map + authoritative resize only for the host (UI command queue)
    if (host == wid) {
      x11_ui_push_map(wid);

      // After mapping a top-level host, emit an authoritative resize to Swift.
      // This ensures Cocoa host is sized from X11 geometry and avoids relying on transient Cocoa sizes.
      WindowView vw{};
      if (ctx.windows().snapshot(wid, vw)) {
        x11_ui_push_resize(wid, (int32_t)vw.w, (int32_t)vw.h);
      }
    }
    
    // 3) Notify client if selected (always on wid, not host)
    if (const WindowView* vw = ctx.window(wid)) {
      const bool wantExp = vw->mapped && ((vw->event_mask & (1u << 15)) != 0);
      const bool wantCfg =               ((vw->event_mask & (1u << 17)) != 0);
      if (wantExp || wantCfg) {
        ctx.transport().queueNotify(wid, wantCfg, wantExp);
      }
    }

    // 4) If anything drew before presentable, flush once now (route to host)
    if (host != 0) {
      if (ctx.windows().consumeDirtyIfReady(host)) {
        x11_requests_push_damage(host);
      }
    }
  }
  
  
void WindowOps::handleMapSubwindows(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t parent = br.readU32();
  br.skip(br.remaining());
  if (parent == 0) return;

  // Rootless host for this subtree.
  const uint32_t host = ctx.windows().topLevelAncestorOf(parent);

  // Map all descendants (not including the parent itself)
  auto desc = ctx.windows().descendantsOf(parent);

  // Track whether anything changed and whether we should flush host dirty once.
  bool anyMapped = false;

  for (uint32_t xid : desc) {
    // Skip if already mapped
    if (const WindowView* before = ctx.window(xid)) {
      if (before->mapped) continue;
    }

    anyMapped = true;

    // 1) Authoritative state
    ctx.windows().setMapped(xid, true);

    // 2) Notify client if selected (per-window)
    if (const WindowView* vw = ctx.window(xid)) {
      const bool wantExp = vw->mapped && ((vw->event_mask & (1u << 15)) != 0); // ExposureMask
      const bool wantCfg =               ((vw->event_mask & (1u << 17)) != 0); // StructureNotifyMask
      if (wantExp || wantCfg) {
        ctx.transport().queueNotify(xid, wantCfg, wantExp);
      }
    }
  }

  // 4) Cocoa visibility: only mark and map the host (once).
  bool hostWasMapped = false;
  if (host != 0) {
    if (const WindowView* hv0 = ctx.window(host)) hostWasMapped = hv0->mapped;
  }
  if (anyMapped && host != 0) {
    ctx.windows().markDirty(host);
    
    // If the host itself is being mapped elsewhere, this is harmless (Swift side should de-dupe).
    if (!hostWasMapped) x11_ui_push_map(host);

    // Also push authoritative resize for the host (same reasoning as MapWindow)
    WindowView hv{};
    if (ctx.windows().snapshot(host, hv)) {
      x11_ui_push_resize(host, (int32_t)hv.w, (int32_t)hv.h);
    }
    
    // If anything was dirty, flush once now if host is ready.
    if (ctx.windows().consumeDirtyIfReady(host)) {
      x11_requests_push_damage(host);
    }
  }
}
  
  
void WindowOps::handleUnmapWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());
  if (wid == 0) return;

  // 1) Update authoritative table
  ctx.windows().setMapped(wid, false);

  // 2) Rootless: route "needs repaint later" to host, not child.
  const uint32_t host = ctx.windows().topLevelAncestorOf(wid);
  if (host != 0) {
    ctx.windows().markDirty(host);
  } else {
    // fallback (shouldn't happen)
    ctx.windows().markDirty(wid);
  }

  // 3) Swift/UI visibility: only top-level host should actually be hidden.
  // If wid is a child, Cocoa window should remain; compositing will omit it.
  if (host == wid) {
    x11_requests_push_unmap(wid);
  } else if (host != 0) {
    // Optional: if you maintain per-xid Swift state, you may still want to tell Swift
    // "child is unmapped" for hit-testing, but do NOT hide the NSWindow.
    // For now, keep it simple: no Swift event for children.
  }
}
  
// -----------------------------
// UnmapSubwindows (major 11)
// -----------------------------
void WindowOps::handleUnmapSubwindows(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  // Request body: CARD32 parent
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t parent = br.readU32();
  br.skip(br.remaining());
  if (parent == 0) return;

  // Unmap all descendants (not including the parent itself)
  auto desc = ctx.windows().descendantsOf(parent);

  for (uint32_t xid : desc) {
    const WindowView* before = ctx.window(xid);
    if (before && !before->mapped) continue; // already unmapped

    // 1) Authoritative state
    ctx.windows().setMapped(xid, false);

    // 2) Rootless: ensure the host will repaint to reflect the child disappearing
    const uint32_t host = ctx.windows().topLevelAncestorOf(xid);
    if (host != 0) ctx.windows().markDirty(host);

    // 3) (Optional future) UnmapNotify to client if StructureNotifyMask selected.
    // You probably don’t have UnmapNotify event wiring yet, so omit for now.
  }

  // If the parent itself is a top-level host, you may optionally mark it dirty too
  // (helpful if descendantsOf() missed something).
  const uint32_t hostP = ctx.windows().topLevelAncestorOf(parent);
  if (hostP != 0) ctx.windows().markDirty(hostP);
}
  
  
// Host resized native surface for wid; update server truth + backing FB + notify + redraw.
// Called on server/protocol thread.
void applyRootlessResize(XProtoContext& ctx, uint32_t wid, int32_t w_px, int32_t h_px)
{
  if (wid == 0) return;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  const WindowView* vw0 = ctx.window(wid);
  if (!vw0) return;

  const uint16_t old_w = vw0->w;
  const uint16_t old_h = vw0->h;

  uint16_t new_w = (uint16_t)((w_px > 65535) ? 65535 : w_px);
  uint16_t new_h = (uint16_t)((h_px > 65535) ? 65535 : h_px);
  if (new_w == 0) new_w = 1;
  if (new_h == 0) new_h = 1;
  if (new_w == old_w && new_h == old_h) return;

#ifndef NDEBUG
  const uint32_t host = ctx.windows().topLevelAncestorOf(wid);
  if (host != wid) {
    ctx.tracef("[PROTO BUG] applyRootlessResize called on non-host wid=0x%08X host=0x%08X\n",
               (unsigned)wid, (unsigned)host);
  }
#endif

  // 1) Resize backing FB (C owns pixels).
  FB_RESIZE(wid, new_w, new_h, "applyRootlessResize on wid (step 1)");
  //x11_backend_fb_resize(wid, new_w, new_h);

  // 2a) Update authoritative geometry in C++.
  ctx.windows().setGeometryRootlessHost(wid, vw0->x, vw0->y, new_w, new_h);
  
  // 2b) Deliver ConfigureNotify / Expose to the *host* after a host-driven resize,
  // so clients like xterm recompute their grid and resize subwindows.
  if (const x11::WindowView* vw = ctx.window(wid /*host*/)) {
    const bool wantCfg = ((vw->event_mask & (1u << 17)) != 0); // StructureNotifyMask
    const bool wantExp = ((vw->event_mask & (1u << 15)) != 0); // ExposureMask
    if (wantCfg || wantExp) {
      ctx.transport().queueNotify(wid, wantCfg, wantExp);
    }
  }

  // Optional: if your policy is to clamp child windows on host resize, do it here.
  // ctx.windows().clampDescendantsToParent(wid);

  // 3) Sync descendant FB sizes to authoritative geometry.
  {
    auto kids = ctx.windows().descendantsOf(wid);
    for (uint32_t kid : kids) {
      WindowView kv{};
      if (!ctx.windows().snapshot(kid, kv)) continue;

      const uint16_t kw = kv.w ? kv.w : 1;
      const uint16_t kh = kv.h ? kv.h : 1;
      FB_RESIZE(kid, kw, kh, "applyRootlessResize on descendant kid (step 3)");
      //x11_backend_fb_resize(kid, kw, kh);
    }
  }

  // 4) Notify owning client if selected.
  if (const WindowView* vw = ctx.window(wid)) {
    const bool wantCfg = ((vw->event_mask & (1u << 17)) != 0);
    const bool wantExp = (vw->mapped && ((vw->event_mask & (1u << 15)) != 0));
    if (wantCfg || wantExp) ctx.transport().queueNotify(wid, wantCfg, wantExp);
  }

  // 5) Redraw/present (gated).
  damageOrDirty(ctx, wid);
}
  
} // namespace x11
