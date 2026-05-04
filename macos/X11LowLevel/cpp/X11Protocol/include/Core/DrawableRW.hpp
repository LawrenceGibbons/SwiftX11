//
//  DrawableRW.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/30/26.
//

#pragma once

#include <cstdint>

#include "Core/DrawableSurfaceRegistry.hpp"

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
  //
  // ── Concurrency (C2, v1.19.35.47-dbg) ─────────────────────────────────
  // For WINDOW drawables, DrawableRW now embeds a DrawableSurfaceRegistry
  // ReadHandle (`readLock`).  The handle holds a shared lock on the
  // surface registry from the moment resolveDrawableRW returns true
  // until the DrawableRW goes out of scope — which is exactly the
  // duration of the caller's draw op.  Reallocation (writer lock) blocks
  // while any draw op is in flight.  This makes DrawableRW move-only.
  // (C3 will move buffer ownership from Swift to C++; until then, the
  // shared lock protects against registry mutation but Swift's ARC can
  // still free the Foundation.Data buffer — that's fixed in C3.)

struct DrawableRW {
  // ── Move-only: ReadHandle is non-copyable ──
  DrawableRW() = default;
  DrawableRW(DrawableRW&&) noexcept = default;
  DrawableRW& operator=(DrawableRW&&) noexcept = default;
  DrawableRW(const DrawableRW&) = delete;
  DrawableRW& operator=(const DrawableRW&) = delete;
  ~DrawableRW() = default;

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

  // ---- Surface read-lock (C2) ----
  // Held for the lifetime of this DrawableRW for WINDOW drawables.
  // Empty for PIXMAP drawables (pixmaps have their own storage in
  // PixmapTable; not subject to live-resize replacement).
  DrawableSurfaceRegistry::ReadHandle readLock;

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
// On success for WINDOW drawables, `out.readLock` holds the surface registry's
// shared lock; the caller must keep `out` alive until the draw op completes.
bool resolveDrawableRW(XProtoContext& ctx, uint32_t drawable, DrawableRW& out);

} // namespace x11

