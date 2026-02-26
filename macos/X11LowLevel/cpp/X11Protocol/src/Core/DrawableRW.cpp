//
//  DrawableRW.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/31/26.
//

#include "Core/DrawableRW.hpp"

#include <cstddef>
#include <cstdint>

#include "Core/XProtoContext.hpp"
#include "Core/PixmapTable.hpp"
#include "Core/WindowTable.hpp"
#include "Core/SurfaceDesc.hpp"
#include "Core/DrawableSurfaceRegistry.hpp"


namespace x11 {

static bool computeOffsetInHost(WindowTable& wt,
                                uint32_t host,
                                uint32_t xid,
                                int32_t& outX,
                                int32_t& outY)
{
  outX = 0;
  outY = 0;
  if (host == 0 || xid == 0) return false;
  if (host == xid) return true;

  uint32_t cur = xid;
  for (int hop = 0; hop < 256; hop++) {
    WindowView vw{};
    if (!wt.snapshot(cur, vw)) return false;

    // accumulate this node's offset in its parent
    outX += (int32_t)vw.x;
    outY += (int32_t)vw.y;

    if (vw.parent_xid == 0) return false;
    if (vw.parent_xid == host) return true;

    cur = vw.parent_xid;
  }
  return false;
}

bool resolveDrawableRW(XProtoContext& ctx,
                       uint32_t drawable,
                       DrawableRW& out)
{
  out = {};

  // ---------------- WINDOW ----------------
  if (ctx.windows().exists(drawable)) {
    // Determine host (top-level) window.
    const uint32_t host = ctx.windows().topLevelAncestorOf(drawable);
    const uint32_t key  = host ? host : drawable;

    // Get drawable geometry (for clipping).
    WindowView dv{};
    if (!ctx.windows().snapshot(drawable, dv)) {
      fprintf(stderr, "[RESOLVE] drawable=0x%08X FAIL snapshot\n", (unsigned)drawable);
      return false;
    }

    // Resolve via Swift-owned surface registry (published per host).
    x11::SurfaceDesc s{};
    if (!ctx.surfaces().get(key, s) ||
        s.ptr == nullptr || s.bytesPerRow == 0 ||
        s.w == 0 || s.h == 0)
    {
      // No Swift surface yet for this host.  This is expected early in
      // the window lifecycle (between CreateWindow and the first
      // ensureHostSurface on the Swift side).  Drawing will succeed once
      // the surface is registered.
      fprintf(stderr,
              "[RESOLVE] drawable=0x%08X host=0x%08X FAIL no Swift surface\n",
              (unsigned)drawable, (unsigned)key);
      return false;
    }

    if ((s.bytesPerRow & 3u) != 0) {
      fprintf(stderr, "[RESOLVE] drawable=0x%08X host=0x%08X FAIL bpr alignment (%u)\n",
              (unsigned)drawable, (unsigned)key, (unsigned)s.bytesPerRow);
      return false;
    }
    const uint32_t stridePx = s.bytesPerRow / 4u;
    if (stridePx == 0) return false;

    // Compute drawable origin in host coords.
    int32_t ox = 0, oy = 0;
    if (!computeOffsetInHost(ctx.windows(), key, drawable, ox, oy)) {
      fprintf(stderr, "[RESOLVE] drawable=0x%08X host=0x%08X FAIL offset computation\n",
              (unsigned)drawable, (unsigned)key);
      return false;
    }
    if (ox < 0 || oy < 0) {
      fprintf(stderr, "[RESOLVE] drawable=0x%08X host=0x%08X FAIL negative offset (%d,%d)\n",
              (unsigned)drawable, (unsigned)key, (int)ox, (int)oy);
      return false;
    }

    // Clamp drawable dims to backing surface bounds (defensive).
    uint32_t maxW = (ox < (int32_t)s.w) ? (uint32_t)((int32_t)s.w - ox) : 0u;
    uint32_t maxH = (oy < (int32_t)s.h) ? (uint32_t)((int32_t)s.h - oy) : 0u;

    // Host uses surface size; ALL children clip to their own WindowView
    // geometry.  This prevents ClearArea / PolyFillRectangle on one child
    // from scribbling over sibling children (e.g., VT100 clearing into
    // the scrollbar area).
    uint16_t effW = 0, effH = 0;
    if (drawable == key) {
      // Host drawable: match the backing surface dimensions
      effW = (uint16_t)std::min<uint32_t>(s.w, maxW); // maxW==s.w when ox==0
      effH = (uint16_t)std::min<uint32_t>(s.h, maxH);
    } else {
      // Child drawable: ALWAYS clip to its own geometry.
#ifndef NDEBUG
      fprintf(stderr,
              "[RESOLVE] child drawable=0x%08X host=0x%08X parent=0x%08X "
              "xy=(%d,%d) wh=%ux%u surf=%ux%u maxWH=%ux%u\n",
              (unsigned)drawable, (unsigned)key, (unsigned)dv.parent_xid,
              (int)dv.x, (int)dv.y, (unsigned)dv.w, (unsigned)dv.h,
              (unsigned)s.w, (unsigned)s.h,
              (unsigned)maxW, (unsigned)maxH);
#endif

      effW = (uint16_t)std::min<uint32_t>(dv.w, maxW);
      effH = (uint16_t)std::min<uint32_t>(dv.h, maxH);
    }
    if (effW == 0 || effH == 0) {
      fprintf(stderr, "[RESOLVE] drawable=0x%08X host=0x%08X FAIL effWH=%ux%u\n",
              (unsigned)drawable, (unsigned)key, (unsigned)effW, (unsigned)effH);
      return false;
    }

    uint32_t* base = static_cast<uint32_t*>(s.ptr);
    uint32_t* shifted = base + (size_t)oy * (size_t)stridePx + (size_t)ox;

    out.pixels32 = shifted;
    out.w = effW;
    out.h = effH;
    out.stridePixels = stridePx;

    out.isWindow = true;
    out.isPixmap = false;
    out.depth = 32;

    out.backingXid = key;
    out.backingPixels32 = base;
    out.backingStridePixels = stridePx;
    out.offsetX = ox;
    out.offsetY = oy;

#ifndef NDEBUG
    fprintf(stderr,
            "[RESOLVE] OK drawable=0x%08X host=0x%08X path=SURFACE "
            "effWH=%ux%u stride=%u off=(%d,%d) surfWH=%ux%u\n",
            (unsigned)drawable, (unsigned)key,
            (unsigned)out.w, (unsigned)out.h,
            (unsigned)out.stridePixels,
            (int)out.offsetX, (int)out.offsetY,
            (unsigned)s.w, (unsigned)s.h);
#endif

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
    out.stridePixels = pw;

    out.isWindow = false;
    out.isPixmap = true;
    out.depth = 32;

    out.backingXid = drawable;
    out.backingPixels32 = pix;
    out.backingStridePixels = pw;
    out.offsetX = 0;
    out.offsetY = 0;
    return true;
  }

  return false;
}

} // namespace x11
