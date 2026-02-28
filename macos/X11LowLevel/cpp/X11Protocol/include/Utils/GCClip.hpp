#pragma once

#include "Core/GCTable.hpp"
#include <algorithm>
#include <cstdint>

namespace x11 {

// ---------------------------------------------------------------------------
// GC clip-rectangle utilities
//
// X11 semantics:
//   has_clip == false              → No clipping (draw everywhere in drawable)
//   has_clip == true, rects empty  → Clip everything (draw nothing)
//   has_clip == true, rects > 0    → Draw only within union of rects
//
// clip_x_origin / clip_y_origin offset the clip rects relative to drawable
// origin: effective rect (in drawable coords) = (r.x + clip_x_origin, ...).
// ---------------------------------------------------------------------------

// Intersect two axis-aligned rects [ax0,ax1) x [ay0,ay1) and [bx0,bx1) x [by0,by1).
// Writes intersection into ox0..oy1.  Returns true if non-empty.
static inline bool clipRectIntersect(
    int32_t ax0, int32_t ay0, int32_t ax1, int32_t ay1,
    int32_t bx0, int32_t by0, int32_t bx1, int32_t by1,
    int32_t& ox0, int32_t& oy0, int32_t& ox1, int32_t& oy1)
{
  ox0 = std::max(ax0, bx0);
  oy0 = std::max(ay0, by0);
  ox1 = std::min(ax1, bx1);
  oy1 = std::min(ay1, by1);
  return (ox0 < ox1) && (oy0 < oy1);
}

// Test whether a single pixel at drawable coords (x,y) passes the GC clip.
static inline bool gcPointVisible(const GCState& gc, int32_t x, int32_t y) {
  if (!gc.has_clip) return true;
  // Translate from drawable coords to clip-mask coords
  const int32_t cx = x - (int32_t)gc.clip_x_origin;
  const int32_t cy = y - (int32_t)gc.clip_y_origin;
  for (const auto& r : gc.clip_rects) {
    if (cx >= (int32_t)r.x && cx < (int32_t)r.x + (int32_t)r.w &&
        cy >= (int32_t)r.y && cy < (int32_t)r.y + (int32_t)r.h)
      return true;
  }
  return false;
}

// For a rectangular draw region [dx0,dx1) x [dy0,dy1) in drawable coords,
// iterate over GC clip rects and invoke `fn(cx0, cy0, cx1, cy1)` for each
// non-empty intersection (in drawable coords).
//
// If GC has no clip, invokes fn once with the original rect.
// If GC has empty clip rects (clip everything), fn is never called.
//
// Fn signature: void(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
template<typename Fn>
static inline void gcClipForEachRect(const GCState& gc,
                                     int32_t dx0, int32_t dy0,
                                     int32_t dx1, int32_t dy1,
                                     Fn&& fn)
{
  if (!gc.has_clip) {
    fn(dx0, dy0, dx1, dy1);
    return;
  }
  // has_clip=true: intersect with each clip rect (union of rects)
  for (const auto& r : gc.clip_rects) {
    const int32_t rx0 = (int32_t)r.x + (int32_t)gc.clip_x_origin;
    const int32_t ry0 = (int32_t)r.y + (int32_t)gc.clip_y_origin;
    const int32_t rx1 = rx0 + (int32_t)r.w;
    const int32_t ry1 = ry0 + (int32_t)r.h;

    int32_t ox0, oy0, ox1, oy1;
    if (clipRectIntersect(dx0, dy0, dx1, dy1, rx0, ry0, rx1, ry1,
                          ox0, oy0, ox1, oy1)) {
      fn(ox0, oy0, ox1, oy1);
    }
  }
}

} // namespace x11
