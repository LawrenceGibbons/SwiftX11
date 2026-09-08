//
//  COre/InputRouting.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/15/26.
//

#include "Core/InputRouting.hpp"
#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include <vector>

namespace x11 {

static inline bool isMapped(const WindowView& vw) { return vw.mapped != 0; }

uint32_t pickDeepestMappedWindowAtHostPoint(XProtoContext& ctx, uint32_t host_xid, int32_t host_x, int32_t host_y)
{
  if (host_xid == 0) return 0;

  WindowView hv{};
  if (!ctx.windows().snapshot(host_xid, hv)) return 0;
  if (!isMapped(hv)) return host_xid;

  // Quick reject if pointer not inside host
  if (host_x < 0 || host_y < 0 || host_x >= (int32_t)hv.w || host_y >= (int32_t)hv.h) {
    return host_xid;
  }

  // xorg XYToWindow (dix/events.c): descend from the host, at each level
  // taking the TOPMOST mapped child whose footprint (border included, shape
  // honoured) contains the point, and stop where no child does.  The old
  // pick chose "deepest, first found" over every descendant, so among
  // overlapping siblings the one created first won regardless of stacking:
  // a window mapped or raised over a sibling under the pointer was never
  // the sprite window (no crossings, clicks to the covered sibling) —
  // Phase G, L17 verification (v1.20.0.38).
  uint32_t cur = host_xid;
  int32_t lx = host_x, ly = host_y;          // point in cur's content coordinates
  for (int depth = 0; depth < 64; depth++) {
    const std::vector<uint32_t> kids = ctx.windows().childrenInStackOrder(cur);   // bottom → top
    bool descended = false;
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
      WindowView cv{};
      if (!ctx.windows().snapshot(*it, cv)) continue;
      if (!isMapped(cv)) continue;
      const int32_t bw = (int32_t)cv.border_width;
      const int32_t cx = lx - ((int32_t)cv.x + bw);   // point in the child's content coords
      const int32_t cy = ly - ((int32_t)cv.y + bw);
      if (cx < -bw || cy < -bw || cx >= (int32_t)cv.w + bw || cy >= (int32_t)cv.h + bw) continue;
      if ((cv.input_shaped || cv.bounding_shaped) &&
          !ctx.windows().isInShapeRegion(*it, (int16_t)cx, (int16_t)cy)) continue;
      cur = *it; lx = cx; ly = cy; descended = true;
      break;
    }
    if (!descended) break;
  }
  return cur;
}

} // namespace x11
