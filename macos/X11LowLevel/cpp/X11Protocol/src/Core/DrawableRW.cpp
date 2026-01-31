//
//  DrawableRW.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/31/26.
//

#include "DrawableRW.hpp"

#include "XProtoContext.hpp"
#include "PixmapTable.hpp"
#include "WindowTable.hpp"

// C bridge for window framebuffer access
extern "C" int x11_xproto_window_fb_rw(uint32_t xid,
                                      uint32_t** outPixels,
                                      uint32_t* outW,
                                      uint32_t* outH);

namespace x11 {

bool resolveDrawableRW(XProtoContext& ctx,
                       uint32_t drawable,
                       DrawableRW& out)
{
  out = {}; // reset

  // ------------------------------------------------------------
  // 1) Pixmap (preferred: pure C++)
  // ------------------------------------------------------------
  if (ctx.pixmaps().exists(drawable)) {
    uint16_t w = 0, h = 0;

    // Try depth-1 first
    uint32_t stride = 0;
    if (uint8_t* bits = ctx.pixmaps().mutableBits(drawable, &w, &h, &stride)) {
      out.is_window = false;
      out.bits1 = bits;
      out.stride_bytes = stride;
      out.w = w;
      out.h = h;
      return true;
    }

    // Otherwise depth>1
    if (uint32_t* px = ctx.pixmaps().mutablePixels(drawable, &w, &h)) {
      out.is_window = false;
      out.pixels32 = px;
      out.w = w;
      out.h = h;
      return true;
    }

    return false;
  }

  // ------------------------------------------------------------
  // 2) Window (C framebuffer via bridge)
  // ------------------------------------------------------------
  uint32_t* px = nullptr;
  uint32_t w = 0, h = 0;
  if (x11_xproto_window_fb_rw(drawable, &px, &w, &h) && px) {
    out.is_window = true;
    out.pixels32 = px;
    out.w = (uint16_t)w;
    out.h = (uint16_t)h;
    return true;
  }

  return false;
}

} // namespace x11
