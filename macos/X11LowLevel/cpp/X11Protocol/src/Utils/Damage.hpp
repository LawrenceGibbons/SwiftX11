//
//  Damage.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/4/26.
//

#pragma once
#include <cstdint>

namespace x11 {
class XProtoContext;

inline void damageOrDirty(XProtoContext& ctx, uint32_t wid) {
  ctx.tracef("[DAMAGE_ROUTE] damageOrDirty wid=0x%08X stays unrouted\n", wid);
  // xxx maybe temp  if (ctx.windows().isReadyToPresent(wid)) {
  // xxx maybe temp    x11_requests_push_damage(wid);
  // xxx maybe temp  } else {
  // xxx maybe temp    ctx.windows().markDirty(wid);
  // xxx maybe temp  }
  // xxx maybe temp}
  // xxx temp ----
  if (ctx.windows().exists(wid)) {
    x11_requests_push_damage(wid);
    return;
  }
  // xxx ---- temp
}
  
} // namespace x11
