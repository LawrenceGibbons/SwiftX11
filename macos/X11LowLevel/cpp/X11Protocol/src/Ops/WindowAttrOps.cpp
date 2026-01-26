//
//  WindowAttrOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/26/26.
//

#include "WindowAttrOps.hpp"

#include "XProtoContext.hpp"
#include "WindowTable.hpp"
#include "ByteReader.hpp"
#include "XProtoTransport.hpp"

// Temp -- Update C-side mirror event_mask during transition
#include "XProtoServerBridge.h"

extern "C" {
#include "x11_requests.h"
}


namespace x11 {

WindowAttrOps::WindowAttrOps(XProtoRegistrar& reg) {
  reg.registerMajor( 2, &WindowAttrOps::onMajor, this); // ChangeWindowAttributes
  reg.registerMajor(12, &WindowAttrOps::onMajor, this);  // ConfigureWindow

}

void WindowAttrOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<WindowAttrOps*>(user)->handle(ctx, dc);
}

void WindowAttrOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case 2:  handleChangeWindowAttributes(ctx, dc.seq, dc.br); return;
    case 12: handleConfigureWindow(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[WindowAttrOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// Major=2 request body (after 4-byte header):
//   CARD32 window
//   CARD32 valueMask
//   LISTofCARD32 valueList
//
// We only implement CWEventMask (bit 11) for now.
void WindowAttrOps::handleChangeWindowAttributes(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }

  const uint32_t wid   = br.readU32();
  const uint32_t vmask = br.readU32();

  // Value list is 32-bit items in increasing bit order.
  // We only care about bit 11, but we still must consume correctly.
  uint32_t cur_mask = 0;

  // Start from current value if we have it (nice to keep stable when partial updates happen)
  if (const WindowView* vw = ctx.window(wid)) {
    cur_mask = vw->event_mask;
  }

  for (uint32_t bit = 0; bit < 32 && br.remaining() >= 4; bit++) {
    if ((vmask & (1u << bit)) == 0) continue;
    const uint32_t val = br.readU32();
    if (bit == 11) {
      cur_mask = val; // CWEventMask
    }
  }

  // Consume any trailing bytes (padding or unparsed values if malformed)
  br.skip(br.remaining());

  // If CWEventMask wasn’t present, we do nothing.
  if ((vmask & (1u << 11)) == 0) return;

  // 1) Update C++ authoritative table
  ctx.windows().setEventMask(wid, cur_mask);

  // 2) Keep C mirror in sync until ALL remaining C handlers stop reading w->event_mask
  x11_xproto_c_set_window_event_mask(wid, cur_mask);
}

  void WindowAttrOps::handleConfigureWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
    // Body (after 4-byte header):
    //   CARD32 window
    //   CARD16 valueMask
    //   CARD16 pad
    //   LISTofCARD32 values
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }

    const uint32_t wid   = br.readU32();
    const uint16_t vmask = br.readU16();
    (void)br.readU16(); // pad

    // Pull current values (so “partial configure” keeps the rest)
    int16_t  x = 0, y = 0;
    uint16_t w = 1, h = 1;

    if (const WindowView* vw = ctx.window(wid)) {
      x = vw->x;
      y = vw->y;
      w = vw->w ? vw->w : 1;
      h = vw->h ? vw->h : 1;
    }

    // Values are 32-bit units in bit order 0..15
    for (uint32_t bit = 0; bit < 16; bit++) {
      if ((vmask & (1u << bit)) == 0) continue;
      if (br.remaining() < 4) break;
      const uint32_t v = br.readU32();

      switch (bit) {
        case 0: x = (int16_t)v; break;          // X
        case 1: y = (int16_t)v; break;          // Y
        case 2: w = (uint16_t)v; if (w == 0) w = 1; break; // Width
        case 3: h = (uint16_t)v; if (h == 0) h = 1; break; // Height
        default: break; // ignore others for now
      }
    }

    // Consume any trailing bytes (defensive)
    br.skip(br.remaining());

    // Update authoritative WindowTable
    ctx.windows().setGeometry(wid, x, y, w, h);
    
    // keep C canonical state in sync (this is what makes drawing correct)
    x11_xproto_apply_configure_from_cpp(wid, x, y, w, h, /*resize_fb=*/1);


    // Tell Swift/shim side about configure (existing behavior)
    x11_requests_push_configure(wid, (int32_t)w, (int32_t)h);

    // Queue notifies based on latest window snapshot
    if (const WindowView* vw2 = ctx.window(wid)) {
      const bool wantCfg = ((vw2->event_mask & (1u << 17)) != 0);
      const bool wantExp = ((vw2->event_mask & (1u << 15)) != 0); // don’t gate on mapped here
      if (wantCfg || wantExp) {
        ctx.transport().queueNotify(wid, wantCfg, wantExp);
      }
    }
  }
  
  
} // namespace x11
