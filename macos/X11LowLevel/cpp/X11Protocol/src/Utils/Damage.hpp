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
  
  inline void damageOrDirty(XProtoContext& ctx, uint32_t dstXid)
  {
#ifndef NDEBUG
    ctx.tracef("entering damageOrDirty\n");
#endif
    if (dstXid == 0) return;
    if (!ctx.windows().exists(dstXid)) return; // pixmap or unknown

    uint32_t target = dstXid;

    if (!ctx.windows().isReadyToPresent(dstXid)) {
      target = ctx.windows().topLevelAncestorOf(dstXid);
      if (target == 0) return;
    }

#ifndef NDEBUG
  ctx.tracef("[DAMAGE_OR_DIRTY] dst=0x%08X exists=%d ready(dst)=%d -> host=0x%08X ready(host)=%d action=%s\n",
             dstXid,
             ctx.windows().exists(dstXid) ? 1 : 0,
             ctx.windows().exists(dstXid) ? (ctx.windows().isReadyToPresent(dstXid) ? 1 : 0) : 0,
             target,
             ctx.windows().isReadyToPresent(target) ? 1 : 0,
             ctx.windows().isReadyToPresent(target) ? "push_damage" : "mark_dirty");
#endif
    
    if (ctx.windows().isReadyToPresent(target)) {
      x11_requests_push_damage(target);
    } else {
      ctx.windows().markDirty(target);
    }
  }
  
} // namespace x11
