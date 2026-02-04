//  WindowOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#include "WindowOps.hpp"

#include "XProtoContext.hpp"
#include "ByteReader.hpp"
#include "XProtoTransport.hpp"
#include "WindowTable.hpp"
#include "x11_requests.h"
#include "HostResize.hpp"

// temp
#include "XProtoServerBridge.h"
#include <cstdio>   // snprintf

namespace x11 {

static inline void damageOrDirty(XProtoContext& ctx, uint32_t wid) {
  if (ctx.windows().isReadyToPresent(wid)) x11_requests_push_damage(wid);
  else ctx.windows().markDirty(wid);
}
  

//// Host resized native surface for wid; update server truth + FB + notify + redraw.
//// Preconditions: called on server/protocol thread (same as old function name implied).
//static void applyRootlessResize(XProtoContext& ctx, uint32_t wid, int32_t w_px, int32_t h_px)
//{
//  if (wid == 0) return;
//  if (w_px < 1) w_px = 1;
//  if (h_px < 1) h_px = 1;
//
//  const WindowView* vw0 = ctx.window(wid);
//  if (!vw0) return;
//
//  const uint16_t old_w = vw0->w;
//  const uint16_t old_h = vw0->h;
//
//  const uint16_t new_w = (uint16_t)((w_px > 65535) ? 65535 : w_px);
//  const uint16_t new_h = (uint16_t)((h_px > 65535) ? 65535 : h_px);
//
//  if (new_w == old_w && new_h == old_h) return;
//
//  // 1) Resize C backing pixels first (so subsequent drawing uses correct buffer).
//  x11_backend_fb_resize(wid, new_w, new_h);
//
//  // 2) Update authoritative geometry in WindowTable (x/y unchanged).
//  ctx.windows().setGeometry(wid, vw0->x, vw0->y, new_w, new_h);
//
//  // 3) Notify owning client if they selected masks.
//  if (const WindowView* vw = ctx.window(wid)) {
//    const bool wantCfg = ((vw->event_mask & (1u << 17)) != 0);              // StructureNotifyMask
//    const bool wantExp = (vw->mapped && ((vw->event_mask & (1u << 15)) != 0)); // ExposureMask
//    if (wantCfg || wantExp) ctx.transport().queueNotify(wid, wantCfg, wantExp);
//  }
//
//  // 4) Ensure UI redraw (gated by ready/presentable).
//  damageOrDirty(ctx, wid);
//}

  
WindowOps::WindowOps(XProtoRegistrar& reg) {
  reg.registerMajor(1,  &WindowOps::onMajor, this);  // CreateWindow
  reg.registerMajor(4,  &WindowOps::onMajor, this);  // DestroyWindow
  reg.registerMajor(8,  &WindowOps::onMajor, this);  // MapWindow
  reg.registerMajor(9,  &WindowOps::onMajor, this);  // MapSubwindows
  reg.registerMajor(10, &WindowOps::onMajor, this);  // UnmapWindow
}

void WindowOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<WindowOps*>(user)->handle(ctx, dc);
}

void WindowOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case 1:  handleCreateWindow(ctx, dc.seq, dc.minor /*depth*/, dc.br); return;
    case 4:  handleDestroyWindow(ctx, dc.seq, dc.br); return;
    case 8:  handleMapWindow(ctx, dc.seq, dc.br); return;
    case 9:  handleMapSubwindows(ctx, dc.seq, dc.br); return;
    case 10: handleUnmapWindow(ctx, dc.seq, dc.br); return;
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
  fprintf(stderr, "[SwiftX11] end_session: destroy xid=0x%08X\n", (unsigned)wid);
}
  
  
void WindowOps::handleMapWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  // Request body: CARD32 window
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());

  // 1) Update authoritative table
  ctx.windows().setMapped(wid, true);

  // 2) Swift-side event (existing behavior)
  x11_requests_push_map(wid);

  // 3) Expose to client *if client selected ExposureMask*
  // We don't have the C w->event_mask anymore, so query via ctx.window()
  if (const WindowView* vw = ctx.window(wid)) {
    const bool wantExp = vw->mapped && ((vw->event_mask & (1u<<15)) != 0);
    const bool wantCfg =               ((vw->event_mask & (1u<<17)) != 0);
    if (wantExp || wantCfg) {
      ctx.transport().queueNotify(wid, wantCfg, wantExp);
    }
  }

  // 4) If drawing happened before map/presentable, flush once now
  if (ctx.windows().consumeDirtyIfReady(wid)) {
    x11_requests_push_damage(wid);
  }
}

void WindowOps::handleMapSubwindows(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t parent = br.readU32();
  br.skip(br.remaining());

  // Map all descendants (not including the parent itself)
  auto desc = ctx.windows().descendantsOf(parent);
  for (uint32_t xid : desc) {
    // Skip if already mapped
    const WindowView* before = ctx.window(xid);
    if (before && before->mapped) continue;

    // 1) Authoritative state
    ctx.windows().setMapped(xid, true);

    // 2) Swift side map event (rootless visibility)
    x11_requests_push_map(xid);

    // 3) Expose/ConfigureNotify to client if selected
    if (const WindowView* vw = ctx.window(xid)) {
      const bool wantExp = ((vw->event_mask & (1u << 15)) != 0); // ExposureMask
      const bool wantCfg = ((vw->event_mask & (1u << 17)) != 0); // StructureNotifyMask
      if (wantExp || wantCfg) {
        ctx.transport().queueNotify(xid, wantCfg, wantExp);
      }
    }

    // 4) If it drew before being ready-to-present, flush once now
    if (ctx.windows().consumeDirtyIfReady(xid)) {
      x11_requests_push_damage(xid);
    }
  }
}
  
  
void WindowOps::handleUnmapWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());

  ctx.windows().setMapped(wid, false);

  // ensure it will repaint after next map/presentable
  ctx.windows().markDirty(wid);

  x11_requests_push_unmap(wid);
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

  // 1) Resize backing FB (C owns pixels).
  x11_backend_fb_resize(wid, new_w, new_h);

  // 2) Update authoritative geometry in C++ (x/y unchanged).
  ctx.windows().setGeometry(wid, vw0->x, vw0->y, new_w, new_h);

  // 3) Tell Swift to resize surface (size-only; keep existing queue format).
  x11_requests_push_configure(wid, (int32_t)new_w, (int32_t)new_h);

  // 4) Notify owning client if selected.
  if (const WindowView* vw = ctx.window(wid)) {
    const bool wantCfg = ((vw->event_mask & (1u << 17)) != 0);                 // StructureNotifyMask
    const bool wantExp = (vw->mapped && ((vw->event_mask & (1u << 15)) != 0)); // ExposureMask
    if (wantCfg || wantExp) ctx.transport().queueNotify(wid, wantCfg, wantExp);
  }

  // 5) Redraw/present (gated).
  damageOrDirty(ctx, wid);
}
  
} // namespace x11
