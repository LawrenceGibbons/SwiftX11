//
//  BackgroundFill.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 3/7/26.
//
//  Shared utility for tiling a background pixmap into a drawable region.
//  Used by fillWindowBackground, fillWindowBackgroundIfReady, and ClearArea.
//

#pragma once

#include <cstdint>
#include <cstdio>
#include "Core/DrawableRW.hpp"
#include "Core/PixmapTable.hpp"
#include "Core/XProtoContext.hpp"

namespace x11 {

// Tile a pixmap into a rectangular region of a DrawableRW.
// X11 spec: background pixmap origin aligns with the window origin,
// so tile coordinates are simply (pixel_x % tile_w, pixel_y % tile_h).
//
// Parameters:
//   ctx       - protocol context (for pixmap lookup)
//   pixmapXid - the background pixmap XID
//   dst       - resolved drawable to fill into
//   x0,y0     - start of fill region (in drawable-local coords)
//   x1,y1     - end of fill region (exclusive)
//
// Returns true if the pixmap was found and tiling was performed.
static inline bool tilePixmapFill(XProtoContext& ctx, uint32_t pixmapXid,
                                  DrawableRW& dst,
                                  int x0, int y0, int x1, int y1)
{
    PixmapView pv{};
    if (!ctx.pixmaps().snapshot(pixmapXid, pv)) {
#ifndef NDEBUG
        fprintf(stderr, "[TILE_BG] pixmap=0x%08X NOT FOUND in PixmapTable\n",
                (unsigned)pixmapXid);
#endif
        return false;
    }

    // Background pixmaps must be 32bpp (matching window depth).
    // For depth-1 stipple pixmaps used as backgrounds, X11 clients
    // create a 32bpp tiled version. If we only have bits, skip.
    if (!pv.pixels || pv.w == 0 || pv.h == 0) {
#ifndef NDEBUG
        fprintf(stderr, "[TILE_BG] pixmap=0x%08X depth=%u %ux%u pixels=%p bits=%p — no 32bpp data\n",
                (unsigned)pixmapXid, (unsigned)pv.depth,
                (unsigned)pv.w, (unsigned)pv.h,
                (const void*)pv.pixels, (const void*)pv.bits);
#endif
        return false;
    }

    const int32_t tw = (int32_t)pv.w;
    const int32_t th = (int32_t)pv.h;

#ifndef NDEBUG
    fprintf(stderr, "[TILE_BG] pixmap=0x%08X %ux%u depth=%u filling [%d,%d)->[%d,%d) p0=0x%08X\n",
            (unsigned)pixmapXid, (unsigned)pv.w, (unsigned)pv.h, (unsigned)pv.depth,
            x0, y0, x1, y1, (unsigned)pv.pixels[0]);
#endif

    if (dst.numOccluded > 0) {
        for (int yy = y0; yy < y1; yy++) {
            uint32_t* row = dst.pixels32 + (size_t)yy * (size_t)dst.stridePixels;
            int32_t ty = yy % th;
            if (ty < 0) ty += th;
            for (int xx = x0; xx < x1; xx++) {
                if (dst.isOccluded(xx, yy)) continue;
                int32_t tx = xx % tw;
                if (tx < 0) tx += tw;
                row[xx] = pv.pixels[(size_t)ty * (size_t)pv.w + (size_t)tx] | 0xFF000000u;
            }
        }
    } else {
        for (int yy = y0; yy < y1; yy++) {
            uint32_t* row = dst.pixels32 + (size_t)yy * (size_t)dst.stridePixels;
            int32_t ty = yy % th;
            if (ty < 0) ty += th;
            for (int xx = x0; xx < x1; xx++) {
                int32_t tx = xx % tw;
                if (tx < 0) tx += tw;
                row[xx] = pv.pixels[(size_t)ty * (size_t)pv.w + (size_t)tx] | 0xFF000000u;
            }
        }
    }

    return true;
}

} // namespace x11
