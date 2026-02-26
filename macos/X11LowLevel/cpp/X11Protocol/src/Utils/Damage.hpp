//
//  Damage.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/4/26.
//

#pragma once
#include <cstdint>
#include "UI/UICommandQueue.hpp"

namespace x11 {
  class XProtoContext;

  static inline uint32_t hostForPresent(x11::XProtoContext& ctx, uint32_t xid) {
    if (xid == 0) return 0;
    if (!ctx.windows().exists(xid)) return 0;
    const uint32_t host = ctx.windows().topLevelAncestorOf(xid);
    return host ? host : xid;
  }

  static inline void damageOrDirty(x11::XProtoContext& ctx, uint32_t drawableXid) {
    if (drawableXid == 0) return;
    if (!ctx.windows().exists(drawableXid)) return;

    const uint32_t host = hostForPresent(ctx, drawableXid);
    if (host == 0) {
#ifdef X11_TRACE_VERBOSE
      fprintf(stderr, "[DAMAGE] drawable=0x%08X SKIP host=0\n", (unsigned)drawableXid);
#endif
      return;
    }

    ctx.windows().markDirty(host);

    if (ctx.windows().consumeDirtyIfReady(host)) {
#ifdef X11_TRACE_VERBOSE
      fprintf(stderr, "[DAMAGE] drawable=0x%08X host=0x%08X -> FLUSH (ready)\n",
              (unsigned)drawableXid, (unsigned)host);
#endif
      ctx.ui().push(x11::UICommand{
        x11::UICommand::Type::Damage,
        /*xid=*/host,
        /*parent=*/0,
        /*w_px=*/0, /*h_px=*/0,
        /*cursor_xid=*/0,
        /*shape=*/0,
        /*title_utf8=*/nullptr
      });
    } else {
#ifdef X11_TRACE_VERBOSE
      fprintf(stderr, "[DAMAGE] drawable=0x%08X host=0x%08X -> DEFERRED (not ready)\n",
              (unsigned)drawableXid, (unsigned)host);
#endif
    }
  }

} // namespace x11
