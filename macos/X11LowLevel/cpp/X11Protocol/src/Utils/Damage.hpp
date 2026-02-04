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
  if (ctx.windows().isReadyToPresent(wid)) {
    x11_requests_push_damage(wid);
  } else {
    ctx.windows().markDirty(wid);
  }
}
} // namespace x11
