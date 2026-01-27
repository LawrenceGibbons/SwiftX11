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


// temp
#include "x11_window_set_mapped.h"
#include "XProtoWindowBridge.h"
#include "XProtoWindowLookupBridge.h"
#include "XProtoWindowCBridge.h"
#include <cstdio>   // snprintf

namespace x11 {

WindowOps::WindowOps(XProtoRegistrar& reg) {
  reg.registerMajor(1,  &WindowOps::onMajor, this);  // CreateWindow
//  reg.registerMajor(4,  &WindowOps::onMajor, this);  // DestroyWindow
  reg.registerMajor(8,  &WindowOps::onMajor, this);  // MapWindow
//  reg.registerMajor(9,  &WindowOps::onMajor, this);  // MapSubwindows
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
  if (!x11_xproto_c_create_window_slot(wid, parent, x, y, wpx, hpx, event_mask, owner_fd, &dirty)) {
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
  br.skip(br.remaining());
  // ctx.tracef("[WindowOps] DestroyWindow (stub)\n");
}

void WindowOps::handleMapWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  // Request body: CARD32 window
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());

  // 1) Update authoritative table
  ctx.windows().setMapped(wid, true);
  // temp
  x11_xproto_c_window_set_mapped(wid, true);

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
  br.skip(br.remaining());
  // ctx.tracef("[WindowOps] MapSubwindows (stub)\n");
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
  
} // namespace x11
