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
#include "XProtoWindowBridge.h"
#include "XProtoWindowLookupBridge.h"

// temp
#include "x11_window_set_mapped.h"

namespace x11 {

WindowOps::WindowOps(XProtoRegistrar& reg) {
//  reg.registerMajor(1,  &WindowOps::onMajor, this);  // CreateWindow
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

void WindowOps::handleCreateWindow(XProtoContext& ctx, uint16_t /*seq*/, uint8_t /*depth*/, ByteReader& br) {
  br.skip(br.remaining());
  // ctx.tracef("[WindowOps] CreateWindow (stub)\n");
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
