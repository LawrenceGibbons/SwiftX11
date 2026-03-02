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
#include "Utils/TraceDefs.hpp"

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
#if X11_TRACE_PRESENT_ENABLED
    fprintf(stderr, "[BG_FILL] wid=0x%08X SKIP resolve failed\n", (unsigned)wid);
#endif
    return;
  }
  if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) return;

  const uint32_t bg = vw.background_pixel;

#if X11_TRACE_PRESENT_ENABLED
  fprintf(stderr, "[BG_FILL] wid=0x%08X bg=0x%08X wh=%ux%u stride=%u\n",
          (unsigned)wid, (unsigned)bg,
          (unsigned)dst.w, (unsigned)dst.h, (unsigned)dst.stridePixels);
#endif

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

// Draw the server-side border around a child window.
// X11 spec: the server paints borders in the PARENT's drawable, surrounding the
// child's interior (drawable) area.  In our rootless model, all drawing goes into
// the host (top-level) surface.
//
// The child's (x,y) position is the outer border corner in parent coordinates.
// Total footprint: (x, y) to (x + w + 2*bw - 1, y + h + 2*bw - 1).
// The drawable interior starts at (x + bw, y + bw).
static void fillWindowBorder(XProtoContext& ctx, uint32_t childXid) {
  WindowView cv{};
  if (!ctx.windows().snapshot(childXid, cv)) return;
  if (cv.border_width == 0) return;

  // Can't draw borders for host windows (parent=root has no surface)
  if (cv.parent_xid == 0 || cv.parent_xid == 1) return;

  // Resolve parent's drawable (will be in the host surface)
  DrawableRW parentDst{};
  if (!resolveDrawableRW(ctx, cv.parent_xid, parentDst)) {
#if X11_TRACE_PRESENT_ENABLED
    fprintf(stderr, "[BORDER] childXid=0x%08X SKIP parent resolve failed\n", (unsigned)childXid);
#endif
    return;
  }
  if (!parentDst.pixels32 || parentDst.w == 0 || parentDst.h == 0) return;

  const int32_t bw = (int32_t)cv.border_width;
  const uint32_t bp = cv.border_pixel;

  // Border outer rect in parent drawable coords
  const int32_t bx = (int32_t)cv.x;
  const int32_t by = (int32_t)cv.y;
  const int32_t totalW = (int32_t)cv.w + 2 * bw;
  const int32_t totalH = (int32_t)cv.h + 2 * bw;

  // Lambda to fill a rect clipped to parent drawable
  auto fillRect = [&](int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > (int32_t)parentDst.w) rw = (int32_t)parentDst.w - rx;
    if (ry + rh > (int32_t)parentDst.h) rh = (int32_t)parentDst.h - ry;
    if (rw <= 0 || rh <= 0) return;

    for (int32_t y = ry; y < ry + rh; y++) {
      uint32_t* row = parentDst.pixels32 + (size_t)y * parentDst.stridePixels;
      for (int32_t x = rx; x < rx + rw; x++) {
        row[x] = bp;
      }
    }
  };

  // Top border strip
  fillRect(bx, by, totalW, bw);
  // Bottom border strip
  fillRect(bx, by + bw + (int32_t)cv.h, totalW, bw);
  // Left border strip
  fillRect(bx, by + bw, bw, (int32_t)cv.h);
  // Right border strip
  fillRect(bx + bw + (int32_t)cv.w, by + bw, bw, (int32_t)cv.h);

  // Damage the border region on the host
  if (parentDst.isWindow) {
    damageOrDirty(ctx, cv.parent_xid, bx, by, totalW, totalH);
  }

#if X11_TRACE_PRESENT_ENABLED
  fprintf(stderr, "[BORDER] childXid=0x%08X parent=0x%08X bw=%d bp=0x%08X at=(%d,%d) total=%dx%d\n",
          (unsigned)childXid, (unsigned)cv.parent_xid,
          (int)bw, (unsigned)bp,
          (int)bx, (int)by, (int)totalW, (int)totalH);
#endif
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
  reg.registerMajor(x11::opcode::CreateWindow,      &WindowOps::onMajor, this);  // CreateWindow
  reg.registerMajor(x11::opcode::DestroyWindow,     &WindowOps::onMajor, this);  // DestroyWindow
  reg.registerMajor(x11::opcode::DestroySubwindows, &WindowOps::onMajor, this);  // 5
  reg.registerMajor(x11::opcode::ReparentWindow,    &WindowOps::onMajor, this);  // 7
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
    case x11::opcode::CreateWindow      :  handleCreateWindow(ctx, dc.seq, dc.minor /*depth*/, dc.br); return;
    case x11::opcode::DestroyWindow     :  handleDestroyWindow(ctx, dc.seq, dc.br); return;
    case x11::opcode::DestroySubwindows :  handleDestroySubwindows(ctx, dc.seq, dc.br); return;
    case x11::opcode::ReparentWindow    :  handleReparentWindow(ctx, dc.seq, dc.br); return;
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

  const uint16_t borderWidth = br.readU16();
  (void)br.readU16(); // class
  (void)br.readU32(); // visual
  const uint32_t vmask = br.readU32();

  uint32_t event_mask = 0;
  uint32_t bg_pixel = 0;
  bool     has_bg_pixel = false;
  bool     parent_relative = false; // CWBackPixmap=1 (ParentRelative)
  uint32_t border_pixel_raw = 0;
  bool     has_border_pixel = false;
  // Consume values for *all* bits set; record bit 0 (CWBackPixmap), bit 1 (CWBackPixel),
  // bit 3 (CWBorderPixel), bit 11 (CWEventMask).
  for (uint32_t bit = 0; bit < 32; bit++) {
    if (!(vmask & (1u << bit))) continue;
    if (br.remaining() < 4) break;
    const uint32_t val = br.readU32();
    if (bit == 0) { // CWBackPixmap
      if (val == 1) { // ParentRelative
        parent_relative = true;
      }
      // val == 0 → None (no background, which is our default)
      // val > 1  → pixmap ID (not supported yet)
    }
    if (bit == 1) { // CWBackPixel
      // Map X11 pixel value to ARGB8888 (force alpha opaque)
      if (val == 0)       bg_pixel = 0xFF000000u;       // black
      else if (val == 1)  bg_pixel = 0xFFFFFFFFu;       // white
      else                bg_pixel = 0xFF000000u | (val & 0x00FFFFFFu);
      has_bg_pixel = true;
    }
    if (bit == 3) { // CWBorderPixel
      border_pixel_raw = val;
      has_border_pixel = true;
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
  if (borderWidth > 0) {
    ctx.windows().setBorderWidth(wid, borderWidth);
  }
  if (has_border_pixel) {
    // Map X11 pixel value to ARGB8888
    uint32_t bp_argb;
    if (border_pixel_raw == 0)       bp_argb = 0xFF000000u;
    else if (border_pixel_raw == 1)  bp_argb = 0xFFFFFFFFu;
    else                             bp_argb = 0xFF000000u | (border_pixel_raw & 0x00FFFFFFu);
    ctx.windows().setBorderPixel(wid, bp_argb);
  }
  if (has_bg_pixel) {
    ctx.windows().setBackgroundPixel(wid, bg_pixel);
#if X11_TRACE_LIFECYCLE_ENABLED
    fprintf(stderr, "[CreateWindow] wid=0x%08X bg_pixel=0x%08X\n",
            (unsigned)wid, (unsigned)bg_pixel);
#endif
  } else if (parent_relative) {
    // CWBackPixmap=ParentRelative: inherit the nearest ancestor's background_pixel.
    bool resolved = ctx.windows().resolveParentRelativeBackground(wid);
#if X11_TRACE_LIFECYCLE_ENABLED
    fprintf(stderr, "[CreateWindow] wid=0x%08X ParentRelative resolved=%d\n",
            (unsigned)wid, (int)resolved);
#endif
    (void)resolved;
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

#if X11_TRACE_LIFECYCLE_ENABLED
  fprintf(stderr, "[LIFECYCLE] CreateWindow wid=0x%08X parent=0x%08X xy=(%d,%d) wh=%ux%u evmask=0x%08X bg=%s\n",
          (unsigned)wid, (unsigned)parent,
          (int)x, (int)y, (unsigned)wpx, (unsigned)hpx,
          (unsigned)event_mask,
          has_bg_pixel ? "yes" : "no");
#endif
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

// -------------------- DestroySubwindows (opcode 5)
void WindowOps::handleDestroySubwindows(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());
  if (wid == 0) return;

  // Get all descendants in BFS order, then destroy deepest first
  auto desc = ctx.windows().descendantsOf(wid);

  // Reverse: destroy deepest children first (leaf → root)
  for (auto it = desc.rbegin(); it != desc.rend(); ++it) {
    const uint32_t child = *it;
    ctx.windows().erase(child);
    x11_ui_push_destroy(child);
  }

#if X11_TRACE_LIFECYCLE_ENABLED
  fprintf(stderr, "[LIFECYCLE] DestroySubwindows parent=0x%08X destroyed=%zu children\n",
          (unsigned)wid, desc.size());
#endif
}

// -------------------- ReparentWindow (opcode 7)
void WindowOps::handleReparentWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }
  const uint32_t wid       = br.readU32();
  const uint32_t newParent = br.readU32();
  const int16_t  x         = (int16_t)br.readU16();
  const int16_t  y         = (int16_t)br.readU16();
  br.skip(br.remaining());
  if (wid == 0) return;

  // Snapshot current state
  WindowView vw{};
  if (!ctx.windows().snapshot(wid, vw)) return;
  const bool wasMapped = vw.mapped;

  // If mapped, unmap first (X11 spec: ReparentWindow unmaps if mapped)
  if (wasMapped) {
    ctx.windows().setMapped(wid, false);
  }

  // Reparent in WindowTable
  ctx.windows().reparent(wid, newParent, x, y);

  // Send ReparentNotify (event type 21) to the window itself
  {
    const uint16_t evSeq = ctx.transport().nextEventSeq();
    uint8_t ev[32] = {};
    ev[0] = 21; // ReparentNotify
    wire::wr16_le(ev + 2, evSeq);
    wire::wr32_le(ev + 4, wid);       // event = window itself
    wire::wr32_le(ev + 8, wid);       // window
    wire::wr32_le(ev + 12, newParent); // parent
    wire::wr16_le(ev + 16, (uint16_t)x);
    wire::wr16_le(ev + 18, (uint16_t)y);
    ev[20] = 0; // override-redirect = false
    (void)ctx.transport().sendEvent32(wid, ev);
  }

  // If was mapped, remap
  if (wasMapped) {
    ctx.windows().setMapped(wid, true);
    fillWindowBorder(ctx, wid);
    fillWindowBackground(ctx, wid);
    sendInitialExposeNow(ctx, wid);
  }

#if X11_TRACE_LIFECYCLE_ENABLED
  fprintf(stderr, "[LIFECYCLE] ReparentWindow wid=0x%08X newParent=0x%08X xy=(%d,%d) wasMapped=%d\n",
          (unsigned)wid, (unsigned)newParent, (int)x, (int)y, (int)wasMapped);
#endif
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

#if X11_TRACE_LIFECYCLE_ENABLED
    {
      WindowView mv{};
      ctx.windows().snapshot(wid, mv);
      fprintf(stderr, "[LIFECYCLE] MapWindow wid=0x%08X host=0x%08X isHost=%d parent=0x%08X xy=(%d,%d) wh=%ux%u\n",
              (unsigned)wid, (unsigned)host, (int)(host == wid),
              (unsigned)mv.parent_xid,
              (int)mv.x, (int)mv.y, (unsigned)mv.w, (unsigned)mv.h);
    }
#endif

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

    // 3) Fill window border + background (X11 spec: server paints before Expose).
    fillWindowBorder(ctx, wid);
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

#if X11_TRACE_LIFECYCLE_ENABLED
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
#endif

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

    // Fill border + background (X11 spec: server paints before Expose)
    fillWindowBorder(ctx, xid);
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
  
  // 2) Deliver ConfigureNotify + Expose to the *host* after a host-driven resize,
  // so clients like xterm recompute their grid and resize subwindows.
  if (const x11::WindowView* vw = ctx.window(wid /*host*/)) {
    const bool wantCfg = ((vw->event_mask & (1u << 17)) != 0); // StructureNotifyMask
    const bool wantExp = ((vw->event_mask & (1u << 15)) != 0); // ExposureMask
    if (wantCfg || wantExp) {
      ctx.transport().queueNotify(wid, wantCfg, wantExp);
    }
  }

  // 3) Children are NOT re-exposed here — their geometry hasn't changed yet.
  // When xterm processes the host ConfigureNotify, it sends ConfigureWindow
  // for each child.  The ConfigureWindow handler fills the child's background
  // at the new position and sends a direct Expose, ensuring prompt repaint.

  // 4) Redraw/present (gated). Resize → full window repaint.
  damageOrDirty(ctx, wid, 0, 0, (int32_t)new_w, (int32_t)new_h);
}
  
} // namespace x11
