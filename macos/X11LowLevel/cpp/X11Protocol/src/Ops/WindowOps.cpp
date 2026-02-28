//  WindowOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#include "Ops/WindowOps.hpp"

#include "Core/XProtoContext.hpp"
#include "Utils/ByteReader.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Core/WindowTable.hpp"
#include "Core/DrawableRW.hpp"
#include "Core/HostResize.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "UI/UICommandQueue.hpp"
#include "Utils/WireEvents.hpp"

// util
#include "Damage.hpp"

// bridge
#include "XProtoServerBridge.h"
#include <cstdio>   // snprintf
extern "C" {
#include "SwiftX11Bridge.h"
}

namespace x11 {

// Fill a window's drawable area with its background_pixel (if set).
// This implements the X11 spec requirement: the server should paint the window's
// background before delivering Expose events.
static void fillWindowBackground(XProtoContext& ctx, uint32_t wid) {
  WindowView vw{};
  if (!ctx.windows().snapshot(wid, vw)) return;
  if (!vw.has_background_pixel) return;

  DrawableRW dst{};
  if (!resolveDrawableRW(ctx, wid, dst)) {
    fprintf(stderr, "[BG_FILL] wid=0x%08X SKIP resolve failed\n", (unsigned)wid);
    return;
  }
  if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) return;

  const uint32_t bg = vw.background_pixel;

  fprintf(stderr, "[BG_FILL] wid=0x%08X bg=0x%08X wh=%ux%u stride=%u\n",
          (unsigned)wid, (unsigned)bg,
          (unsigned)dst.w, (unsigned)dst.h, (unsigned)dst.stridePixels);

  for (uint16_t y = 0; y < dst.h; y++) {
    uint32_t* row = dst.pixels32 + (size_t)y * (size_t)dst.stridePixels;
    for (uint16_t x = 0; x < dst.w; x++) {
      row[x] = bg;
    }
  }

  // Damage the host so the fill is presented.
  if (dst.isWindow) {
    damageOrDirty(ctx, wid, 0, 0, (int32_t)dst.w, (int32_t)dst.h);
  }
}

static inline void sendInitialExposeNow(x11::XProtoContext& ctx, uint32_t wid) {
  if (wid == 0) return;

  x11::WindowView vw{};
  if (!ctx.windows().snapshot(wid, vw)) return;

  // Full-window expose. Use a nonzero seq field; nextEventSeq() is ideal.
  const uint16_t seq = ctx.transport().nextEventSeq();

  auto ev = x11::wireev::buildExpose(seq,
                                     wid,
                                     /*x=*/0, /*y=*/0,
                                     vw.w, vw.h,
                                     /*count=*/0);

  (void)ctx.transport().sendEvent32(wid, ev.data());
}
  
WindowOps::WindowOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::CreateWindow,    &WindowOps::onMajor, this);  // CreateWindow
  reg.registerMajor(x11::opcode::DestroyWindow,   &WindowOps::onMajor, this);  // DestroyWindow
  reg.registerMajor(x11::opcode::MapWindow,       &WindowOps::onMajor, this);  // MapWindow
  reg.registerMajor(x11::opcode::MapSubwindows,   &WindowOps::onMajor, this);  // MapSubwindows
  reg.registerMajor(x11::opcode::UnmapWindow  ,   &WindowOps::onMajor, this);  // UnmapWindow
  reg.registerMajor(x11::opcode::UnmapSubwindows, &WindowOps::onMajor, this);  // UnmapSubwindows
}

void WindowOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<WindowOps*>(user)->handle(ctx, dc);
}

void WindowOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::CreateWindow    :  handleCreateWindow(ctx, dc.seq, dc.minor /*depth*/, dc.br); return;
    case x11::opcode::DestroyWindow   :  handleDestroyWindow(ctx, dc.seq, dc.br); return;
    case x11::opcode::MapWindow       :  handleMapWindow(ctx, dc.seq, dc.br); return;
    case x11::opcode::MapSubwindows   :  handleMapSubwindows(ctx, dc.seq, dc.br); return;
    case x11::opcode::UnmapWindow     : handleUnmapWindow(ctx, dc.seq, dc.br); return;
    case x11::opcode::UnmapSubwindows : handleUnmapSubwindows(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[WindowOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// -------------------- CreateWindow
void WindowOps::handleCreateWindow(XProtoContext& ctx, uint16_t seq, uint8_t depth, ByteReader& br)
{
  if (br.remaining() < 28) { br.skip(br.remaining()); return; }

  const uint32_t wid    = br.readU32();
  const uint32_t parent = br.readU32();
  const int16_t  x      = (int16_t)br.readU16();
  const int16_t  y      = (int16_t)br.readU16();
  const uint16_t wpx    = br.readU16();
  const uint16_t hpx    = br.readU16();

  (void)br.readU16(); // borderWidth
  (void)br.readU16(); // class
  (void)br.readU32(); // visual
  const uint32_t vmask = br.readU32();

  uint32_t event_mask = 0;
  uint32_t bg_pixel = 0;
  bool     has_bg_pixel = false;
  // Consume values for *all* bits set; record bit 1 (CWBackPixel) and bit 11 (CWEventMask).
  for (uint32_t bit = 0; bit < 32; bit++) {
    if (!(vmask & (1u << bit))) continue;
    if (br.remaining() < 4) break;
    const uint32_t val = br.readU32();
    if (bit == 1) { // CWBackPixel
      // Map X11 pixel value to ARGB8888 (force alpha opaque)
      if (val == 0)       bg_pixel = 0xFF000000u;       // black
      else if (val == 1)  bg_pixel = 0xFFFFFFFFu;       // white
      else                bg_pixel = 0xFF000000u | (val & 0x00FFFFFFu);
      has_bg_pixel = true;
    }
    if (bit == 11) event_mask = val;
  }
  br.skip(br.remaining());

  // ---- VALIDATION (multi-client correctness) ----
  // 0) width/height must be nonzero
  if (wpx == 0 || hpx == 0) {
    ctx.transport().sendErrorCore(x11::error::BadValue, seq, wid, x11::opcode::CreateWindow);
    return;
  }

  // 1) wid must be in this client's id space
  if (!ctx.transport().clientOwnsXid(wid)) {
    ctx.transport().sendErrorCore(x11::error::BadIDChoice, seq, wid, x11::opcode::CreateWindow);
    return;
  }

  // 2) wid must be unused
  x11::WindowView tmp{};
  if (ctx.windows().snapshot(wid, tmp)) {
    ctx.transport().sendErrorCore(x11::error::BadIDChoice, seq, wid, x11::opcode::CreateWindow);
    return;
  }

  // 3) parent must exist (root=1 is always valid in your model)
  if (parent != 1) {
    if (!ctx.windows().snapshot(parent, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::CreateWindow);
      return;
    }
    // optional: enforce same-owner parent (good idea once multi-client)
    if (tmp.owner_fd != ctx.transport().clientFd()) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::CreateWindow);
      return;
    }
  }

  // ---- CREATE (server state only) ----
  const int owner_fd = ctx.transport().clientFd();
  ctx.windows().upsert(wid, parent, x, y, wpx, hpx, event_mask, owner_fd);
  if (has_bg_pixel) {
    ctx.windows().setBackgroundPixel(wid, bg_pixel);
    fprintf(stderr, "[CreateWindow] wid=0x%08X bg_pixel=0x%08X\n",
            (unsigned)wid, (unsigned)bg_pixel);
  }
  ctx.windows().setMapped(wid, false);
  ctx.windows().setPresentable(wid, false);

  // ---- SURFACE OWNERSHIP: Swift-only ----
  // No C framebuffer allocation.  Swift owns all WINDOW backing stores.
  // Host (top-level) windows get their own Swift surface via ensureHostSurface.
  // Child windows draw into the host surface at their offset (via resolveDrawableRW).
  //
  // Queue a UI command so Swift creates the Cocoa window (host) or
  // notes the child (for event routing).  Surface allocation is Swift's job.
  char title[64];
  snprintf(title, sizeof(title), "xid=0x%08X", (unsigned)wid);

  ctx.ui().pushCreate(wid, parent, title, (int32_t)wpx, (int32_t)hpx);

  // Optional: mark dirty so first present/expose happens when mapped/presentable.
  ctx.windows().markDirty(wid);

  // Always-on lifecycle trace for debugging child window issues.
  fprintf(stderr, "[LIFECYCLE] CreateWindow wid=0x%08X parent=0x%08X xy=(%d,%d) wh=%ux%u evmask=0x%08X bg=%s\n",
          (unsigned)wid, (unsigned)parent,
          (int)x, (int)y, (unsigned)wpx, (unsigned)hpx,
          (unsigned)event_mask,
          has_bg_pixel ? "yes" : "no");
}


// -------------------- DestroyWindow
void WindowOps::handleDestroyWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  // Request body: CARD32 window
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());

  if (wid == 0) return;

  // 1) Authoritative C++ state
  ctx.windows().erase(wid);

  // 2) Swift/UI teardown event path
  x11_ui_push_destroy(wid);
}
  
  
  void WindowOps::handleMapWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t wid = br.readU32();
    br.skip(br.remaining());
    if (wid == 0) return;

    // 1) Update authoritative table
    ctx.windows().setMapped(wid, true);

    // Rootless rule: only top-level host windows drive Cocoa.
    const uint32_t host = ctx.windows().topLevelAncestorOf(wid);

    // Always-on lifecycle trace
    {
      WindowView mv{};
      ctx.windows().snapshot(wid, mv);
      fprintf(stderr, "[LIFECYCLE] MapWindow wid=0x%08X host=0x%08X isHost=%d parent=0x%08X xy=(%d,%d) wh=%ux%u\n",
              (unsigned)wid, (unsigned)host, (int)(host == wid),
              (unsigned)mv.parent_xid,
              (int)mv.x, (int)mv.y, (unsigned)mv.w, (unsigned)mv.h);
    }

    // 2) Swift-side map + authoritative resize only for the host (UI command queue)
    if (host == wid) {
      x11_ui_push_map(wid);

      // After mapping a top-level host, emit an authoritative resize to Swift.
      // This ensures Cocoa host is sized from X11 geometry and avoids relying on transient Cocoa sizes.
      WindowView vw{};
      if (ctx.windows().snapshot(wid, vw)) {
        x11_ui_push_resize(wid, (int32_t)vw.w, (int32_t)vw.h);
      }
    }

    // 3) Fill window with background_pixel (X11 spec: server paints background before Expose).
    fillWindowBackground(ctx, wid);

    // 4) Queue an initial full-window Expose on map (bring-up).
    WindowView vw_snap{};
    if (ctx.windows().snapshot(wid, vw_snap)) {
      ctx.transport().queueExposeRect(wid, 0, 0, vw_snap.w, vw_snap.h, 0);
    } else {
      ctx.transport().queueExposeRect(wid, 0, 0, 1, 1, 0);
    }

    // 5) Notify: configure if selected; ALWAYS request expose flush for bring-up.
    // (EventOps will decide whether to honor mask, but we’ll add a bring-up override.)
    bool wantCfg = false;
    if (const WindowView* vw = ctx.window(wid)) {
      wantCfg = ((vw->event_mask & x11::mask::StructureNotify) != 0);
    }
    ctx.transport().queueNotify(wid, /*wantConfigure=*/wantCfg, /*wantExpose=*/true);


    // 6) If anything drew before presentable, flush once now (route to host)
    if (host != 0) {
      x11::WindowView mv2{};
      if (ctx.windows().snapshot(host, mv2)) {
        x11_shared_damage_union(host, 0, 0, (int32_t)mv2.w, (int32_t)mv2.h);
        x11_ui_push_damage(host, 0, 0, (int32_t)mv2.w, (int32_t)mv2.h);
      }
    }
  }
  
  
void WindowOps::handleMapSubwindows(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t parent = br.readU32();
  br.skip(br.remaining());
  if (parent == 0) return;

  // Rootless host for this subtree.
  const uint32_t host = ctx.windows().topLevelAncestorOf(parent);

  // Map all descendants (not including the parent itself)
  auto desc = ctx.windows().descendantsOf(parent);

  fprintf(stderr, "[LIFECYCLE] MapSubwindows parent=0x%08X host=0x%08X numDesc=%zu\n",
          (unsigned)parent, (unsigned)host, desc.size());
  for (uint32_t xid : desc) {
    WindowView dv{};
    bool ok = ctx.windows().snapshot(xid, dv);
    fprintf(stderr, "[LIFECYCLE]   child=0x%08X mapped=%d parent=0x%08X xy=(%d,%d) wh=%ux%u\n",
            (unsigned)xid, ok ? (int)dv.mapped : -1,
            ok ? (unsigned)dv.parent_xid : 0u,
            ok ? (int)dv.x : 0, ok ? (int)dv.y : 0,
            ok ? (unsigned)dv.w : 0u, ok ? (unsigned)dv.h : 0u);
  }

  // Track whether anything changed and whether we should flush host dirty once.
  bool anyMapped = false;

  for (uint32_t xid : desc) {
    // Skip if already mapped
    if (const WindowView* before = ctx.window(xid)) {
      if (before->mapped) continue;
    }

    anyMapped = true;

    // 1) Authoritative state
    ctx.windows().setMapped(xid, true);

    // Fill background (X11 spec: server paints background before Expose)
    fillWindowBackground(ctx, xid);

    // Queue initial Expose rect (bring-up)
    WindowView vw_snap{};
    if (ctx.windows().snapshot(xid, vw_snap)) {
      ctx.transport().queueExposeRect(xid, 0, 0, vw_snap.w, vw_snap.h, 0);
    } else {
      ctx.transport().queueExposeRect(xid, 0, 0, 1, 1, 0);
    }

    // Queue notify: configure if selected; ALWAYS request expose flush for bring-up
    bool wantCfg = false;
    if (const WindowView* vw = ctx.window(xid)) {
      wantCfg = ((vw->event_mask & x11::mask::StructureNotify) != 0);
    }
    ctx.transport().queueNotify(xid, /*wantConfigure=*/wantCfg, /*wantExpose=*/true);
  }
  

  // 4) Cocoa visibility: only mark and map the host (once).
  bool hostWasMapped = false;
  if (host != 0) {
    if (const WindowView* hv0 = ctx.window(host)) hostWasMapped = hv0->mapped;
  }
  if (anyMapped && host != 0) {
    ctx.windows().markDirty(host);
    
    // If the host itself is being mapped elsewhere, this is harmless (Swift side should de-dupe).
    if (!hostWasMapped) x11_ui_push_map(host);

    // Also push authoritative resize for the host (same reasoning as MapWindow)
    WindowView hv{};
    if (ctx.windows().snapshot(host, hv)) {
      x11_ui_push_resize(host, (int32_t)hv.w, (int32_t)hv.h);
    }
    
    // Write full-window damage to shared accumulator and signal.
    {
      x11::WindowView mv3{};
      if (ctx.windows().snapshot(host, mv3)) {
        x11_shared_damage_union(host, 0, 0, (int32_t)mv3.w, (int32_t)mv3.h);
        x11_ui_push_damage(host, 0, 0, (int32_t)mv3.w, (int32_t)mv3.h);
      }
    }
  }
}
  
  
void WindowOps::handleUnmapWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());
  if (wid == 0) return;

  // 1) Update authoritative table
  ctx.windows().setMapped(wid, false);

  // 2) Rootless: route "needs repaint later" to host, not child.
  const uint32_t host = ctx.windows().topLevelAncestorOf(wid);
  if (host != 0) {
    ctx.windows().markDirty(host);
  } else {
    // fallback (shouldn't happen)
    ctx.windows().markDirty(wid);
  }

  // 3) Swift/UI visibility: only top-level host should actually be hidden.
  // If wid is a child, Cocoa window should remain; compositing will omit it.
  if (host == wid) {
    x11_ui_push_unmap(wid);
  } else if (host != 0) {
    // Optional: if you maintain per-xid Swift state, you may still want to tell Swift
    // "child is unmapped" for hit-testing, but do NOT hide the NSWindow.
    // For now, keep it simple: no Swift event for children.
  }
}
  
// -----------------------------
// UnmapSubwindows (major 11)
// -----------------------------
void WindowOps::handleUnmapSubwindows(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  // Request body: CARD32 parent
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t parent = br.readU32();
  br.skip(br.remaining());
  if (parent == 0) return;

  // Unmap all descendants (not including the parent itself)
  auto desc = ctx.windows().descendantsOf(parent);

  for (uint32_t xid : desc) {
    const WindowView* before = ctx.window(xid);
    if (before && !before->mapped) continue; // already unmapped

    // 1) Authoritative state
    ctx.windows().setMapped(xid, false);

    // 2) Rootless: ensure the host will repaint to reflect the child disappearing
    const uint32_t host = ctx.windows().topLevelAncestorOf(xid);
    if (host != 0) ctx.windows().markDirty(host);

    // 3) (Optional future) UnmapNotify to client if StructureNotifyMask selected.
    // You probably don’t have UnmapNotify event wiring yet, so omit for now.
  }

  // If the parent itself is a top-level host, you may optionally mark it dirty too
  // (helpful if descendantsOf() missed something).
  const uint32_t hostP = ctx.windows().topLevelAncestorOf(parent);
  if (hostP != 0) ctx.windows().markDirty(hostP);
}
  
  
// Host resized native surface for wid; update server geometry + notify clients + redraw.
// Called on server/protocol thread.  Swift owns the backing surface.
void applyRootlessResize(XProtoContext& ctx, uint32_t wid, int32_t w_px, int32_t h_px)
{
  if (wid == 0) return;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  const WindowView* vw0 = ctx.window(wid);
  if (!vw0) return;

  const uint16_t old_w = vw0->w;
  const uint16_t old_h = vw0->h;

  uint16_t new_w = (uint16_t)((w_px > 65535) ? 65535 : w_px);
  uint16_t new_h = (uint16_t)((h_px > 65535) ? 65535 : h_px);
  if (new_w == 0) new_w = 1;
  if (new_h == 0) new_h = 1;
  if (new_w == old_w && new_h == old_h) return;

#ifndef NDEBUG
  const uint32_t host = ctx.windows().topLevelAncestorOf(wid);
  if (host != wid) {
    ctx.tracef("[PROTO BUG] applyRootlessResize called on non-host wid=0x%08X host=0x%08X\n",
               (unsigned)wid, (unsigned)host);
  }
#endif

  // 1) Update authoritative geometry in C++.
  // (Swift owns the backing surface; no C FB resize needed.)
  ctx.windows().setGeometryRootlessHost(wid, vw0->x, vw0->y, new_w, new_h);
  
  // 2b) Deliver ConfigureNotify / Expose to the *host* after a host-driven resize,
  // so clients like xterm recompute their grid and resize subwindows.
  if (const x11::WindowView* vw = ctx.window(wid /*host*/)) {
    const bool wantCfg = ((vw->event_mask & (1u << 17)) != 0); // StructureNotifyMask
    const bool wantExp = ((vw->event_mask & (1u << 15)) != 0); // ExposureMask
    if (wantCfg || wantExp) {
      ctx.transport().queueNotify(wid, wantCfg, wantExp);
    }
  }

  // 3) Deliver events to direct children.
  // (No C FB resize needed — children draw into the host's Swift surface.)
  {
    auto kids = ctx.windows().descendantsOf(wid);
    for (uint32_t kid : kids) {
      WindowView kv{};
      if (!ctx.windows().snapshot(kid, kv)) continue;

      // Deliver ConfigureNotify + Expose to direct children of the host
      // so clients (xeyes, xterm) know the host resized and can redraw.
      // We do NOT resize children here — the client is responsible for
      // resizing its own children via ConfigureWindow.
      if (kv.parent_xid == wid) {
        if (const WindowView* ckv = ctx.window(kid)) {
          const bool kidCfg = ((ckv->event_mask & (1u << 17)) != 0); // StructureNotifyMask
          const bool kidExp = (ckv->mapped && ((ckv->event_mask & (1u << 15)) != 0)); // ExposureMask
          if (kidCfg || kidExp) {
            ctx.transport().queueNotify(kid, kidCfg, kidExp);
          }
        }
      }
    }
  }

  // 4) Notify owning client on host if selected.
  if (const WindowView* vw = ctx.window(wid)) {
    const bool wantCfg = ((vw->event_mask & (1u << 17)) != 0);
    const bool wantExp = (vw->mapped && ((vw->event_mask & (1u << 15)) != 0));
    if (wantCfg || wantExp) ctx.transport().queueNotify(wid, wantCfg, wantExp);
  }

  // 5) Redraw/present (gated). Resize → full window repaint.
  damageOrDirty(ctx, wid, 0, 0, (int32_t)new_w, (int32_t)new_h);
}
  
} // namespace x11
