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

// For x11_ui_push_damage (bypasses old C request queue for damage)
extern "C" {
#include "SwiftX11Bridge.h"
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
  // accumulates a bounding-box union in WindowTable, and pushes directly to
  // x11_ui_push_damage (bypassing the old UICommand → C request queue chain).
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
      fprintf(stderr, "[DAMAGE] drawable=0x%08X SKIP host=0\n", (unsigned)drawableXid);
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

    ctx.windows().markDirtyRect(host, hostX, hostY, w, h);

    int32_t rx = 0, ry = 0, rw = 0, rh = 0;
    if (ctx.windows().consumeDirtyRectIfReady(host, rx, ry, rw, rh)) {
#ifdef X11_TRACE_VERBOSE
      fprintf(stderr, "[DAMAGE] drawable=0x%08X host=0x%08X -> FLUSH rect=(%d,%d %dx%d)\n",
              (unsigned)drawableXid, (unsigned)host, (int)rx, (int)ry, (int)rw, (int)rh);
#endif
      x11_ui_push_damage(host, rx, ry, rw, rh);
    } else {
#ifdef X11_TRACE_VERBOSE
      fprintf(stderr, "[DAMAGE] drawable=0x%08X host=0x%08X -> DEFERRED\n",
              (unsigned)drawableXid, (unsigned)host);
#endif
    }
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
      // Fallback: just mark dirty with no rect.
      const uint32_t host = hostForPresent(ctx, drawableXid);
      if (host == 0) return;
      ctx.windows().markDirty(host);

      int32_t rx = 0, ry = 0, rw = 0, rh = 0;
      if (ctx.windows().consumeDirtyRectIfReady(host, rx, ry, rw, rh)) {
        x11_ui_push_damage(host, rx, ry, rw, rh);
      }
    }
  }

} // namespace x11
