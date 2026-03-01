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

  // We’ll pick the deepest window whose rect contains the point.
  uint32_t best = host_xid;
  int bestDepth = -1;

  // Consider all descendants + host.
  std::vector<uint32_t> nodes = ctx.windows().descendantsOf(host_xid);
  nodes.push_back(host_xid);

  auto depthAndLocal = [&](uint32_t xid, int32_t& lx, int32_t& ly, int& depth) -> bool {
    lx = host_x;
    ly = host_y;
    depth = 0;

    // Walk from xid up to host_xid subtracting offsets.
    uint32_t cur = xid;
    int safety = 0;
    while (cur && cur != host_xid) {
      WindowView cv{};
      if (!ctx.windows().snapshot(cur, cv)) return false;
      lx -= (cv.x + cv.border_width);
      ly -= (cv.y + cv.border_width);
      cur = cv.parent_xid;
      depth++;
      if (++safety > 64) return false;
    }
    if (xid != host_xid && cur != host_xid) return false;
    return true;
  };

  for (uint32_t xid : nodes) {
    WindowView vw{};
    if (!ctx.windows().snapshot(xid, vw)) continue;
    if (!isMapped(vw)) continue;

    int32_t lx=0, ly=0; int depth=0;
    if (!depthAndLocal(xid, lx, ly, depth)) continue;
    // Include border region in hit test (border is part of the window's footprint)
    const int32_t bw_i = (int32_t)vw.border_width;
    if (lx < -bw_i || ly < -bw_i || lx >= (int32_t)vw.w + bw_i || ly >= (int32_t)vw.h + bw_i) continue;

    if (depth > bestDepth) {
      best = xid;
      bestDepth = depth;
    }
  }

  return best;
}

} // namespace x11
