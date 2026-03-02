//
//  DrawableRW.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/31/26.
//

#include "Core/DrawableRW.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_set>

#include "Core/XProtoContext.hpp"
#include "Core/PixmapTable.hpp"
#include "Core/WindowTable.hpp"
#include "Core/SurfaceDesc.hpp"
#include "Core/DrawableSurfaceRegistry.hpp"
#include "Utils/TraceDefs.hpp"


namespace x11 {

// Track which child XIDs we've already reported on to avoid log spam.
static std::unordered_set<uint32_t> s_reportedChildResolve;

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

    // accumulate this node's offset in its parent.
    // In X11, (x,y) is the outer border corner; the drawable starts at
    // (x + border_width, y + border_width) in parent coords.
    outX += (int32_t)vw.x + (int32_t)vw.border_width;
    outY += (int32_t)vw.y + (int32_t)vw.border_width;

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
#ifdef X11_TRACE_VERBOSE
      fprintf(stderr, "[RESOLVE] drawable=0x%08X FAIL snapshot\n", (unsigned)drawable);
#endif
      return false;
    }

    // X11 correctness: draws to unmapped windows succeed but produce no
    // visible output.  Since our children share the host surface, we must
    // reject unmapped children here to prevent them from scribbling into
    // the host surface where higher-stacking siblings should be visible.
    // (Host windows are always "mapped" from our perspective once presentable.)
    if (!dv.mapped && drawable != host) {
      return false;
    }

    // Resolve via Swift-owned surface registry (published per host).
    x11::SurfaceDesc s{};
    if (!ctx.surfaces().get(key, s) ||
        s.ptr == nullptr || s.bytesPerRow == 0 ||
        s.w == 0 || s.h == 0)
    {
#ifdef X11_TRACE_VERBOSE
      fprintf(stderr,
              "[RESOLVE] drawable=0x%08X host=0x%08X FAIL no Swift surface\n",
              (unsigned)drawable, (unsigned)key);
#endif
      return false;
    }

    if ((s.bytesPerRow & 3u) != 0) {
#ifdef X11_TRACE_VERBOSE
      fprintf(stderr, "[RESOLVE] drawable=0x%08X host=0x%08X FAIL bpr alignment (%u)\n",
              (unsigned)drawable, (unsigned)key, (unsigned)s.bytesPerRow);
#endif
      return false;
    }
    const uint32_t stridePx = s.bytesPerRow / 4u;
    if (stridePx == 0) return false;

    // Compute drawable origin in host coords.
    int32_t ox = 0, oy = 0;
    if (!computeOffsetInHost(ctx.windows(), key, drawable, ox, oy)) {
#ifdef X11_TRACE_VERBOSE
      fprintf(stderr, "[RESOLVE] drawable=0x%08X host=0x%08X FAIL offset computation\n",
              (unsigned)drawable, (unsigned)key);
#endif
      return false;
    }
    // Clamp negative offsets — child extends beyond host surface origin.
    // X11 allows children at negative positions (e.g., xterm scrollbar at
    // y=-1 to hide a border pixel).  We clamp the offset to 0 and reduce
    // the effective dimensions by the clipped amount.  This shifts the
    // child's drawing origin by at most a few pixels — acceptable for
    // bring-up and visually negligible.
    int32_t skipX = 0, skipY = 0;
    if (ox < 0) { skipX = -ox; ox = 0; }
    if (oy < 0) { skipY = -oy; oy = 0; }

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
      // Child drawable: clip to its own geometry, minus any clipped
      // negative-offset region.
      uint16_t childW = (dv.w > (uint16_t)skipX) ? (uint16_t)(dv.w - (uint16_t)skipX) : 0;
      uint16_t childH = (dv.h > (uint16_t)skipY) ? (uint16_t)(dv.h - (uint16_t)skipY) : 0;

#ifdef X11_TRACE_VERBOSE
      fprintf(stderr,
              "[RESOLVE] child drawable=0x%08X host=0x%08X parent=0x%08X "
              "xy=(%d,%d) wh=%ux%u skip=(%d,%d) childWH=%ux%u surf=%ux%u maxWH=%ux%u\n",
              (unsigned)drawable, (unsigned)key, (unsigned)dv.parent_xid,
              (int)dv.x, (int)dv.y, (unsigned)dv.w, (unsigned)dv.h,
              (int)skipX, (int)skipY,
              (unsigned)childW, (unsigned)childH,
              (unsigned)s.w, (unsigned)s.h,
              (unsigned)maxW, (unsigned)maxH);
#endif

      effW = (uint16_t)std::min<uint32_t>(childW, maxW);
      effH = (uint16_t)std::min<uint32_t>(childH, maxH);

      // ---- Sibling occlusion clipping ----
      // In X11, higher-stacking siblings obscure lower ones.  Since our children
      // share the host surface, we must clip each child's drawable to exclude
      // regions covered by higher-stacking mapped siblings.  This prevents
      // lower-stacking windows from scribbling over higher ones (e.g., xcalc
      // buttons 40-54 at (2,2) hidden beneath the display widget).
      const uint32_t parentXid = dv.parent_xid;
      if (parentXid != 0 && parentXid != 1 && effW > 0 && effH > 0) {
        // Child's content rect in parent coords
        int32_t cx0 = (int32_t)dv.x;
        int32_t cy0 = (int32_t)dv.y;
        int32_t cx1 = (int32_t)dv.x + (int32_t)effW;
        int32_t cy1 = (int32_t)dv.y + (int32_t)effH;

        auto siblings = ctx.windows().childrenInStackOrder(parentXid);
        bool foundSelf = false;
        for (uint32_t sib : siblings) {
          if (sib == drawable) { foundSelf = true; continue; }
          if (!foundSelf) continue; // skip lower-stacking siblings

          WindowView sv{};
          if (!ctx.windows().snapshot(sib, sv)) continue;
          if (!sv.mapped) continue;

          // Sibling's total rect (content + border) in parent coords
          const int32_t sx0 = (int32_t)sv.x - (int32_t)sv.border_width;
          const int32_t sy0 = (int32_t)sv.y - (int32_t)sv.border_width;
          const int32_t sx1 = (int32_t)sv.x + (int32_t)sv.w + (int32_t)sv.border_width;
          const int32_t sy1 = (int32_t)sv.y + (int32_t)sv.h + (int32_t)sv.border_width;

          // Check for overlap
          if (sx0 >= cx1 || sx1 <= cx0 || sy0 >= cy1 || sy1 <= cy0) continue;

          // Sibling overlaps child.  Clip from the edge that yields the
          // largest reduction.  Only clip an edge if the sibling spans the
          // full extent of the child on the perpendicular axis.

          // Right clip (sibling covers right portion, full height)
          if (sx0 > cx0 && sx0 < cx1 && sy0 <= cy0 && sy1 >= cy1) {
            cx1 = sx0;
          }
          // Left clip (sibling covers left portion, full height)
          if (sx1 < cx1 && sx1 > cx0 && sx0 <= cx0 && sy0 <= cy0 && sy1 >= cy1) {
            cx0 = sx1;
          }
          // Bottom clip (sibling covers bottom portion, full width)
          if (sy0 > cy0 && sy0 < cy1 && sx0 <= cx0 && sx1 >= cx1) {
            cy1 = sy0;
          }
          // Top clip (sibling covers top portion, full width)
          if (sy1 < cy1 && sy1 > cy0 && sy0 <= cy0 && sx0 <= cx0 && sx1 >= cx1) {
            cy0 = sy1;
          }

          if (cx0 >= cx1 || cy0 >= cy1) break; // fully clipped
        }

        // Apply clipping: adjust offset and dimensions
        const int32_t clipDx = cx0 - (int32_t)dv.x;
        const int32_t clipDy = cy0 - (int32_t)dv.y;
        const int32_t newW = cx1 - cx0;
        const int32_t newH = cy1 - cy0;

        if (clipDx > 0 || clipDy > 0 || newW < (int32_t)effW || newH < (int32_t)effH) {
          ox += clipDx;
          oy += clipDy;
          effW = (uint16_t)std::max(0, std::min((int32_t)effW - clipDx, newW));
          effH = (uint16_t)std::max(0, std::min((int32_t)effH - clipDy, newH));

          // Re-validate against surface bounds
          if (ox >= (int32_t)s.w || oy >= (int32_t)s.h) { effW = 0; effH = 0; }
        }
      }
    }
    if (effW == 0 || effH == 0) {
#if X11_TRACE_RESOLVE_ENABLED
      fprintf(stderr, "[RESOLVE] drawable=0x%08X host=0x%08X FAIL effWH=%ux%u (dv.wh=%ux%u off=%d,%d surf=%ux%u)\n",
              (unsigned)drawable, (unsigned)key, (unsigned)effW, (unsigned)effH,
              (unsigned)dv.w, (unsigned)dv.h, (int)ox, (int)oy,
              (unsigned)s.w, (unsigned)s.h);
#endif
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

#if X11_TRACE_LIFECYCLE_ENABLED
    // Log the FIRST successful resolve for each child window.
    if (drawable != key && s_reportedChildResolve.find(drawable) == s_reportedChildResolve.end()) {
      s_reportedChildResolve.insert(drawable);
      fprintf(stderr,
              "[LIFECYCLE] FIRST child resolve OK drawable=0x%08X host=0x%08X "
              "effWH=%ux%u stride=%u off=(%d,%d) surfWH=%ux%u parent=0x%08X childXY=(%d,%d)\n",
              (unsigned)drawable, (unsigned)key,
              (unsigned)out.w, (unsigned)out.h,
              (unsigned)out.stridePixels,
              (int)out.offsetX, (int)out.offsetY,
              (unsigned)s.w, (unsigned)s.h,
              (unsigned)dv.parent_xid,
              (int)dv.x, (int)dv.y);
    }
#endif

#ifdef X11_TRACE_VERBOSE
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
