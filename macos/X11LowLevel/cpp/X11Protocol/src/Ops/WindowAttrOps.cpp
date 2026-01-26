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

// Update C-side mirror event_mask during transition
#include "XProtoServerBridge.h"

namespace x11 {

WindowAttrOps::WindowAttrOps(XProtoRegistrar& reg) {
  reg.registerMajor(2, &WindowAttrOps::onMajor, this); // ChangeWindowAttributes
}

void WindowAttrOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<WindowAttrOps*>(user)->handle(ctx, dc);
}

void WindowAttrOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case 2: handleChangeWindowAttributes(ctx, dc.seq, dc.br); return;
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

} // namespace x11
