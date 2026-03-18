#pragma once

#include "Core/GCTable.hpp"
#include "Core/PixmapTable.hpp"
#include <cstdint>
#include <cstddef>
#include <cstdio>

namespace x11 {

// ---------------------------------------------------------------------------
// FillStyle — resolves per-pixel fill color for GC fill_style:
//
//   0 = FillSolid         → foreground color
//   1 = FillTiled         → sample tile pixmap at (px,py) modulo tile size
//   2 = FillStippled      → foreground where stipple bit is 1; skip where 0
//   3 = FillOpaqueStippled → foreground where stipple bit is 1; background where 0
//
// Tile/stipple origin is offset by (ts_x_origin, ts_y_origin).
// ---------------------------------------------------------------------------

// Cached tile/stipple data for use inside inner loops.
// Call fillStyleSetup() once before the fill loop, then use
// fillStyleResolve() per pixel.
struct FillStyleCache {
  uint8_t  style = 0;         // gst.fill_style
  uint32_t fg    = 0xFF000000u;
  uint32_t bg    = 0xFFFFFFFFu;
  int16_t  ts_x  = 0;
  int16_t  ts_y  = 0;

  // Tile (depth != 1, 32bpp)
  const uint32_t* tile_pixels  = nullptr;
  uint16_t        tile_w       = 0;
  uint16_t        tile_h       = 0;

  // Stipple (depth == 1, packed bits)
  const uint8_t*  stip_bits    = nullptr;
  uint32_t        stip_stride  = 0;  // bytes per row
  uint16_t        stip_w       = 0;
  uint16_t        stip_h       = 0;

  bool            valid        = false;  // true if tile/stipple resolved
};

// Populate the cache from GCState and PixmapTable.
// Returns true if the fill style is usable (always true for FillSolid).
static inline bool fillStyleSetup(FillStyleCache& c,
                                  const GCState& gst,
                                  PixmapTable& pix)
{
  c.style = gst.fill_style;
  c.fg    = gst.fg | 0xFF000000u;
  c.bg    = gst.bg | 0xFF000000u;
  c.ts_x  = gst.ts_x_origin;
  c.ts_y  = gst.ts_y_origin;
  c.valid = true;

  if (c.style == 1) {
    // FillTiled — need tile pixmap (32bpp)
    PixmapView pv{};
    bool found = pix.snapshot(gst.tile, pv);
    if (!found || !pv.pixels || pv.w == 0 || pv.h == 0) {
      // Graceful fallback: if the tile is a depth-1 pixmap with bits data,
      // treat it as an opaque stipple (fg where bit=1, bg where bit=0).
      // This handles clients that use FillTiled with a depth-1 stipple bitmap
      // (e.g., Xaw Scrollbar after widget GC recreation).
      if (found && pv.bits && pv.w > 0 && pv.h > 0 && pv.depth == 1) {
        c.style       = 3;  // treat as FillOpaqueStippled
        c.stip_bits   = pv.bits;
        c.stip_stride = pv.stride_bytes;
        c.stip_w      = pv.w;
        c.stip_h      = pv.h;
        c.valid       = true;
        return true;
      }
      c.valid = false;
      c.style = 0;  // fallback to solid
      return false;
    }
    c.tile_pixels = pv.pixels;
    c.tile_w      = pv.w;
    c.tile_h      = pv.h;
  }
  else if (c.style == 2 || c.style == 3) {
    // FillStippled / FillOpaqueStippled — need stipple pixmap (1bpp)
    PixmapView pv{};
    if (!pix.snapshot(gst.stipple, pv) || !pv.bits || pv.w == 0 || pv.h == 0) {
      c.valid = false;
      c.style = 0;  // fallback to solid
      return false;
    }
    c.stip_bits   = pv.bits;
    c.stip_stride = pv.stride_bytes;
    c.stip_w      = pv.w;
    c.stip_h      = pv.h;
  }
  return true;
}

// Result of per-pixel fill resolution.
struct FillResult {
  uint32_t color;
  bool     draw;   // false = skip this pixel (FillStippled, bit is 0)
};

// Resolve fill color at drawable-coordinate (px, py).
// For FillSolid, this is trivial. For tiled/stippled, samples the pattern.
static inline FillResult fillStyleResolve(const FillStyleCache& c,
                                          int32_t px, int32_t py)
{
  switch (c.style) {
    default:
    case 0: // FillSolid
      return { c.fg, true };

    case 1: { // FillTiled
      // Tile coordinates with origin offset, modulo tile size
      int32_t tx = ((px - (int32_t)c.ts_x) % (int32_t)c.tile_w);
      int32_t ty = ((py - (int32_t)c.ts_y) % (int32_t)c.tile_h);
      if (tx < 0) tx += (int32_t)c.tile_w;
      if (ty < 0) ty += (int32_t)c.tile_h;
      uint32_t col = c.tile_pixels[(size_t)ty * (size_t)c.tile_w + (size_t)tx];
      return { col | 0xFF000000u, true };
    }

    case 2: { // FillStippled — draw fg where bit=1, skip where bit=0
      int32_t sx = ((px - (int32_t)c.ts_x) % (int32_t)c.stip_w);
      int32_t sy = ((py - (int32_t)c.ts_y) % (int32_t)c.stip_h);
      if (sx < 0) sx += (int32_t)c.stip_w;
      if (sy < 0) sy += (int32_t)c.stip_h;
      const uint8_t byte = c.stip_bits[(size_t)sy * (size_t)c.stip_stride + (size_t)(sx >> 3)];
      const bool bit = (byte >> (sx & 7)) & 1;
      return { c.fg, bit };
    }

    case 3: { // FillOpaqueStippled — fg where bit=1, bg where bit=0
      int32_t sx = ((px - (int32_t)c.ts_x) % (int32_t)c.stip_w);
      int32_t sy = ((py - (int32_t)c.ts_y) % (int32_t)c.stip_h);
      if (sx < 0) sx += (int32_t)c.stip_w;
      if (sy < 0) sy += (int32_t)c.stip_h;
      const uint8_t byte = c.stip_bits[(size_t)sy * (size_t)c.stip_stride + (size_t)(sx >> 3)];
      const bool bit = (byte >> (sx & 7)) & 1;
      return { bit ? c.fg : c.bg, true };
    }
  }
}

// Convenience: is this fill style just solid (or fell back to solid)?
static inline bool fillStyleIsSolid(const FillStyleCache& c) {
  return c.style == 0;
}

} // namespace x11
