//
//  DrawableRW.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/30/26.
//

#pragma once

#include <cstdint>

namespace x11 {

  // Unified writable drawable view used by DrawOps / ShapeOps / future ops.
  //
  // Exactly one of (pixels32, bits1) is non-null.
  // - pixels32 : 32bpp little-endian pixels (BGRA/XRGB depending on your pipeline; alpha is treated as opaque).
  // - bits1    : packed 1bpp bitmap (LSBFirst).
  //
  // NOTE: pixels32 may be *strided* (bytesPerRow alignment). Always use stridePixels (or bytesPerRow)
  // when stepping rows; do NOT assume row stride == w.
  //
  // If is_window == true, caller must mark dirty / enqueue damage after modification (typically on the host).
  
struct DrawableRW {
  // Pointer to the drawable's (0,0) in its backing buffer.
  // For WINDOW drawables routed to host, this is hostSurface + (offsetY*stride + offsetX).
  uint32_t* pixels32 = nullptr;

  // Drawable dimensions (window or pixmap size) used for clipping.
  uint16_t w = 0;
  uint16_t h = 0;

  // Row stride of the backing buffer in pixels (may be > w).
  uint32_t stridePixels = 0;

  bool isWindow = false;
  bool isPixmap = false;
  uint8_t depth = 32;

  // --- New: backing identity (used for overlap detection / damage routing) ---
  uint32_t  backingXid = 0;             // host XID for windows; drawable for pixmaps
  uint32_t* backingPixels32 = nullptr; // base pointer of backing buffer (host surface)
  uint32_t  backingStridePixels = 0;

  // Drawable origin within backing (host coords). For pixmaps: 0,0.
  int32_t offsetX = 0;
  int32_t offsetY = 0;

  // ---- Sibling occlusion: occluded zones (drawable-local coords) ----
  // Rectangles covered by higher-stacking mapped siblings that drawing
  // operations should NOT write into.  Edge-based clipping in
  // resolveDrawableRW handles cases where a sibling spans a full edge;
  // these rects capture remaining partial overlaps.
  static constexpr int kMaxOccluded = 8;
  struct OccRect { int16_t x, y; uint16_t w, h; };
  OccRect occluded[kMaxOccluded] = {};
  int numOccluded = 0;

  // Fast per-pixel check: is point (px,py) inside any occluded zone?
  // Returns false immediately when numOccluded==0 (common case, zero cost).
  inline bool isOccluded(int32_t px, int32_t py) const {
    for (int i = 0; i < numOccluded; i++) {
      const auto& r = occluded[i];
      if (px >= (int32_t)r.x && px < (int32_t)r.x + (int32_t)r.w &&
          py >= (int32_t)r.y && py < (int32_t)r.y + (int32_t)r.h)
        return true;
    }
    return false;
  }
};
  
class XProtoContext;

// Returns true if drawable is a WINDOW or PIXMAP with writable 32bpp pixels.
bool resolveDrawableRW(XProtoContext& ctx, uint32_t drawable, DrawableRW& out);

} // namespace x11

