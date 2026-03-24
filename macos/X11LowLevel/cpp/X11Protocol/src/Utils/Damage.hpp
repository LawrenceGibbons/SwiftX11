//
//  Damage.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/4/26.
//

#pragma once
#include <cstdint>
#include <cstdio>
#include "Core/XProtoContext.hpp"

extern "C" {
#include "SwiftX11Bridge.h"
#include "Utils/MachTime.hpp"
}

namespace x11 {

  static inline uint32_t hostForPresent(x11::XProtoContext& ctx, uint32_t xid) {
    if (xid == 0) return 0;
    if (!ctx.windows().exists(xid)) return 0;
    const uint32_t host = ctx.windows().topLevelAncestorOf(xid);
    return host ? host : xid;
  }

  // -------------------------------------------------------------------------
  // Rect-aware damage: translates child-local rect to host-surface coords,
  // writes to the shared damage accumulator (read by Swift at present time),
  // and pushes a scheduling signal to the UI command queue.
  //
  // The shared accumulator eliminates the timing gap where C++ pushes damage
  // events to the UI command queue but Swift hasn't drained them yet.
  // -------------------------------------------------------------------------
  static inline void damageOrDirty(x11::XProtoContext& ctx,
                                   uint32_t drawableXid,
                                   int32_t x, int32_t y,
                                   int32_t w, int32_t h)
  {
    if (drawableXid == 0) return;
    if (!ctx.windows().exists(drawableXid)) return;

    const uint32_t host = hostForPresent(ctx, drawableXid);
    if (host == 0) {
#ifdef X11_TRACE_VERBOSE
      TS_FPRINTF("[DAMAGE] drawable=0x%08X SKIP host=0\n", (unsigned)drawableXid);
#endif
      return;
    }

    // Translate child-local rect to host-surface coordinates.
    int32_t hostX = x, hostY = y;
    if (drawableXid != host) {
      int32_t offX = 0, offY = 0;
      if (ctx.windows().absoluteOffsetInHost(host, drawableXid, offX, offY)) {
        hostX += offX;
        hostY += offY;
      }
      // If offset computation fails, use child-local coords as-is (conservative).
    }

    // Write to the shared accumulator (Swift reads this at present time).
    x11_shared_damage_union(host, hostX, hostY, w, h);

    // Push a signal to the UI command queue so Swift schedules a present.
    // The rect values are carried for debug/coalescing but Swift reads the
    // authoritative rect from the shared accumulator at present time.
    x11_ui_push_damage(host, hostX, hostY, w, h);

#ifdef X11_TRACE_VERBOSE
    TS_FPRINTF("[DAMAGE] drawable=0x%08X host=0x%08X rect=(%d,%d %dx%d)\n",
            (unsigned)drawableXid, (unsigned)host, (int)hostX, (int)hostY, (int)w, (int)h);
#endif
  }

  // -------------------------------------------------------------------------
  // No-rect convenience overload: reports full-window damage.
  // Used by callers where computing a precise rect is impractical.
  // -------------------------------------------------------------------------
  static inline void damageOrDirty(x11::XProtoContext& ctx, uint32_t drawableXid)
  {
    if (drawableXid == 0) return;
    if (!ctx.windows().exists(drawableXid)) return;

    // Look up drawable geometry for full-window rect.
    x11::WindowView vw{};
    if (ctx.windows().snapshot(drawableXid, vw)) {
      damageOrDirty(ctx, drawableXid, 0, 0, (int32_t)vw.w, (int32_t)vw.h);
    } else {
      // Fallback: write full-host-window damage and signal.
      const uint32_t host = hostForPresent(ctx, drawableXid);
      if (host == 0) return;

      x11::WindowView hv{};
      if (ctx.windows().snapshot(host, hv)) {
        x11_shared_damage_union(host, 0, 0, (int32_t)hv.w, (int32_t)hv.h);
      }
      x11_ui_push_damage(host, 0, 0, 1, 1);  // signal only
    }
  }

} // namespace x11
