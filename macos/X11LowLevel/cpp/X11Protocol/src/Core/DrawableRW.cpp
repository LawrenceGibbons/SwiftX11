//
//  DrawableRW.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/31/26.
//

#include "Core/DrawableRW.hpp"

#include "Core/XProtoContext.hpp"
#include "Core/PixmapTable.hpp"
#include "Core/WindowTable.hpp"

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
    out = {}; // clear

    if (ctx.windows().exists(drawable)) {
      uint32_t* pix = nullptr;
      uint32_t w = 0, h = 0;
      if (!x11_xproto_window_fb_rw(drawable, &pix, &w, &h) || !pix)
        return false;

      out.pixels32 = pix;
      out.w = (uint16_t)w;
      out.h = (uint16_t)h;
      out.isWindow = true;
      out.isPixmap = false;
      out.depth = 32;   // if that’s what you use
      return true;
    }

    if (ctx.pixmaps().exists(drawable)) {
      uint16_t pw = 0, ph = 0;
      uint32_t* pix = ctx.pixmaps().mutablePixels(drawable, &pw, &ph);
      if (!pix) return false;

      out.pixels32 = pix;
      out.w = pw;
      out.h = ph;
      out.isWindow = false;
      out.isPixmap = true;
      out.depth = 32;   // or whatever pixmap depth is
      return true;
    }

    return false;
  }
  

} // namespace x11
