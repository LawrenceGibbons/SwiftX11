//
//  DrawableRW.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/31/26.
//

//
//  DrawableRW.cpp
//  X11LowLevel
//

#include "Core/DrawableRW.hpp"

#include <cstddef>
#include <cstdint>

#include "Core/XProtoContext.hpp"
#include "Core/PixmapTable.hpp"
#include "Core/WindowTable.hpp"
#include "Core/SurfaceDesc.hpp" // SurfaceDesc / SurfaceFormat (or wherever you defined it)
#include "Core/DrawableSurfaceRegistry.hpp"


// Legacy C bridge for window framebuffer access (temporary fallback)
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

  // ---------------- WINDOW ----------------
  if (ctx.windows().exists(drawable)) {
    // Prefer Swift-owned surface registry.
    x11::SurfaceDesc s{};
    if (ctx.surfaces().get(drawable, s) &&
        s.ptr != nullptr && s.bytesPerRow != 0 &&
        s.w != 0 && s.h != 0)
    {
      // Require 32bpp alignment for now.
      if ((s.bytesPerRow & 3u) != 0) return false;
      const uint32_t stridePx = s.bytesPerRow / 4u;
      if (stridePx == 0) return false;

      out.pixels32 = static_cast<uint32_t*>(s.ptr);
      out.w = s.w;
      out.h = s.h;
      out.stridePixels = stridePx;
      out.isWindow = true;
      out.isPixmap = false;
      out.depth = 32;
      return true;
    }

    // Transitional fallback: old C-backed framebuffer.
    uint32_t* pix = nullptr;
    uint32_t w = 0, h = 0;
    if (!x11_xproto_window_fb_rw(drawable, &pix, &w, &h) || !pix || w == 0 || h == 0) {
      return false;
    }

    out.pixels32 = pix;
    out.w = (uint16_t)w;
    out.h = (uint16_t)h;
    out.stridePixels = w; // old FB path is tightly packed
    out.isWindow = true;
    out.isPixmap = false;
    out.depth = 32;
    return true;
  }

  // ---------------- PIXMAP ----------------
  if (ctx.pixmaps().exists(drawable)) {
    uint16_t pw = 0, ph = 0;
    uint32_t* pix = ctx.pixmaps().mutablePixels(drawable, &pw, &ph);
    if (!pix || pw == 0 || ph == 0) return false;

    out.pixels32 = pix;
    out.w = pw;
    out.h = ph;
    out.stridePixels = pw; // pixmaps are tightly packed in your table today
    out.isWindow = false;
    out.isPixmap = true;
    out.depth = 32;
    return true;
  }

  return false;
}

} // namespace x11
