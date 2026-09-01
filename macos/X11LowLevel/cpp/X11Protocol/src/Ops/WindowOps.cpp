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
#include "Core/XConstants.hpp"
#include "UI/UICommandQueue.hpp"
#include "Utils/WireEvents.hpp"
#include "Core/PropertyTable.hpp"
#include "Core/GrabTable.hpp"
#include "Core/ClipboardAtoms.hpp"
#include "Core/ScreenLayout.hpp"
#include "Utils/WireLE.hpp"

// util
#include "Damage.hpp"
#include "Utils/BackgroundFill.hpp"

// bridge
#include "XProtoServerBridge.h"
#include "Core/XProtoServer.hpp"
#include <cstdio>   // snprintf

// WM-emulation state cleanup on unmap/destroy (defined in XProtoServerBridge.cpp).
extern "C" x11::XProtoServer* x11_proto_bridge_get_server(void);

namespace {

// §2.9 (review 2026-08-31): when the focus window goes away, revert focus
// per the stored SetInputFocus revert-to instead of resetting to None —
// the None reset was the classic "keyboard dead after closing a dialog
// until the user clicks" bug.
//   RevertToParent (2):     focus → nearest mapped ancestor; revert-to
//                           becomes None per spec.
//   RevertToPointerRoot (1): focus → root (PointerRoot approximation).
//   RevertToNone (0):        focus → None (old behavior).
void applyFocusRevert(x11::XProtoContext& ctx, uint32_t parentHint) {
  auto& in = ctx.input();
  uint32_t next = 0;
  if (in.focus_revert_to == 2) {
    uint32_t cur = parentHint;
    for (int hop = 0; hop < 64 && cur && cur != x11::kRootXid; hop++) {
      x11::WindowView pv{};
      if (!ctx.windows().snapshot(cur, pv)) break;
      if (pv.mapped) { next = cur; break; }
      cur = pv.parent_xid;
    }
    if (!next) next = x11::kRootXid;
    in.focus_revert_to = 0; // spec: revert-to becomes None after reverting
  } else if (in.focus_revert_to == 1) {
    next = x11::kRootXid;
  }
  in.setFocusXid(next);
  if (next != 0 && next != x11::kRootXid) {
    uint8_t ev[32] = {};
    ev[0] = 9;  // FocusIn
    ev[1] = 0;  // detail = NotifyAncestor
    x11::wire::wr16_le(ev + 2, ctx.transport().lastSeq());
    x11::wire::wr32_le(ev + 4, next);
    ev[8] = 0;  // mode = NotifyNormal
    ev[9] = 1;  // same-screen
    (void)ctx.transport().sendEvent32(next, ev);
  }
}

} // namespace
extern "C" {
#include "SwiftX11Bridge.h"
}
#include "Utils/TraceDefs.hpp"
#include "Utils/MachTime.hpp"

namespace x11 {

// Fill a window's drawable area with its background (pixel or tiled pixmap).
// This implements the X11 spec requirement: the server should paint the window's
// background before delivering Expose events.
static void fillWindowBackground(XProtoContext& ctx, uint32_t wid) {
  // Check for background pixmap first (takes priority over solid pixel)
  uint32_t bgPixmap = 0;
  if (ctx.windows().resolveBackgroundPixmapForClear(wid, bgPixmap)) {
    DrawableRW dst{};
    if (!resolveDrawableRW(ctx, wid, dst)) return;
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) return;

#if X11_TRACE_PRESENT_ENABLED
    TS_FPRINTF("[BG_FILL] wid=0x%08X pixmap=0x%08X wh=%ux%u stride=%u\n",
            (unsigned)wid, (unsigned)bgPixmap,
            (unsigned)dst.w, (unsigned)dst.h, (unsigned)dst.stridePixels);
#endif

    if (tilePixmapFill(ctx, bgPixmap, dst, 0, 0, (int)dst.w, (int)dst.h)) {
      if (dst.isWindow) {
        damageOrDirty(ctx, wid, 0, 0, (int32_t)dst.w, (int32_t)dst.h);
      }
    }
    return;
  }

  // Fall back to solid-color background
  uint32_t bg = 0;
  if (!ctx.windows().resolveBackgroundForClear(wid, bg)) {
    // No background defined — nothing to fill.
    return;
  }

  DrawableRW dst{};
  if (!resolveDrawableRW(ctx, wid, dst)) {
#if X11_TRACE_PRESENT_ENABLED
    TS_FPRINTF("[BG_FILL] wid=0x%08X SKIP resolve failed\n", (unsigned)wid);
#endif
    return;
  }
  if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) return;

#if X11_TRACE_PRESENT_ENABLED
  TS_FPRINTF("[BG_FILL] wid=0x%08X bg=0x%08X wh=%ux%u stride=%u\n",
          (unsigned)wid, (unsigned)bg,
          (unsigned)dst.w, (unsigned)dst.h, (unsigned)dst.stridePixels);
#endif

  if (dst.numOccluded > 0) {
    for (uint16_t y = 0; y < dst.h; y++) {
      uint32_t* row = dst.pixels32 + (size_t)y * (size_t)dst.stridePixels;
      for (uint16_t x = 0; x < dst.w; x++) {
        if (!dst.isOccluded((int32_t)x, (int32_t)y)) row[x] = bg;
      }
    }
  } else {
    for (uint16_t y = 0; y < dst.h; y++) {
      uint32_t* row = dst.pixels32 + (size_t)y * (size_t)dst.stridePixels;
      for (uint16_t x = 0; x < dst.w; x++) {
        row[x] = bg;
      }
    }
  }

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
    TS_FPRINTF("[BORDER] childXid=0x%08X SKIP parent resolve failed\n", (unsigned)childXid);
#endif
    return;
  }
  if (!parentDst.pixels32 || parentDst.w == 0 || parentDst.h == 0) return;

  const int32_t bw = (int32_t)cv.border_width;
  const uint32_t bp = cv.border_pixel;

  // Border outer rect in parent drawable coords
  // (dv.x, dv.y) is the outer border corner.
  int32_t bx = (int32_t)cv.x;
  int32_t by = (int32_t)cv.y;
  int32_t totalW = (int32_t)cv.w + 2 * bw;
  int32_t totalH = (int32_t)cv.h + 2 * bw;

  // ---- Sibling occlusion for borders ----
  // Clip the border's total rect against higher-stacking siblings' total rects,
  // matching the same clipping logic used for content in resolveDrawableRW.
  {
    int32_t bx1 = bx + totalW;
    int32_t by1 = by + totalH;

    auto siblings = ctx.windows().childrenInStackOrder(cv.parent_xid);
    bool foundSelf = false;
    for (uint32_t sib : siblings) {
      if (sib == childXid) { foundSelf = true; continue; }
      if (!foundSelf) continue;

      WindowView sv{};
      if (!ctx.windows().snapshot(sib, sv)) continue;
      if (!sv.mapped) continue;

      // Sibling's total rect in parent coords
      const int32_t sx0 = (int32_t)sv.x;
      const int32_t sy0 = (int32_t)sv.y;
      const int32_t sx1 = (int32_t)sv.x + (int32_t)sv.w + 2*(int32_t)sv.border_width;
      const int32_t sy1 = (int32_t)sv.y + (int32_t)sv.h + 2*(int32_t)sv.border_width;

      if (sx0 >= bx1 || sx1 <= bx || sy0 >= by1 || sy1 <= by) continue;

      // Right clip
      if (sx0 > bx && sx0 < bx1 && sy0 <= by && sy1 >= by1) { bx1 = sx0; }
      // Left clip
      if (sx1 < bx1 && sx1 > bx && sx0 <= bx && sy0 <= by && sy1 >= by1) { bx = sx1; }
      // Bottom clip
      if (sy0 > by && sy0 < by1 && sx0 <= bx && sx1 >= bx1) { by1 = sy0; }
      // Top clip
      if (sy1 < by1 && sy1 > by && sy0 <= by && sx0 <= bx && sx1 >= bx1) { by = sy1; }

      if (bx >= bx1 || by >= by1) { totalW = 0; totalH = 0; break; }
    }
    totalW = bx1 - bx;
    totalH = by1 - by;
    if (totalW <= 0 || totalH <= 0) return; // fully occluded
  }

  // Recompute inner rect after clipping
  const int32_t innerX = (int32_t)cv.x + bw;
  const int32_t innerY = (int32_t)cv.y + bw;
  const int32_t innerW = (int32_t)cv.w;
  const int32_t innerH = (int32_t)cv.h;

  // Lambda to fill a rect clipped to parent drawable AND to the visible total rect
  auto fillRect = [&](int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    // Clip to visible total rect
    if (rx < bx) { rw -= (bx - rx); rx = bx; }
    if (ry < by) { rh -= (by - ry); ry = by; }
    if (rx + rw > bx + totalW) rw = bx + totalW - rx;
    if (ry + rh > by + totalH) rh = by + totalH - ry;
    // Clip to parent drawable
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

  // Top border strip (uses original child geometry for strip positions)
  fillRect((int32_t)cv.x, (int32_t)cv.y, (int32_t)cv.w + 2*bw, bw);
  // Bottom border strip
  fillRect((int32_t)cv.x, innerY + innerH, (int32_t)cv.w + 2*bw, bw);
  // Left border strip
  fillRect((int32_t)cv.x, innerY, bw, innerH);
  // Right border strip
  fillRect(innerX + innerW, innerY, bw, innerH);

  // Damage the border region on the host
  if (parentDst.isWindow) {
    damageOrDirty(ctx, cv.parent_xid, bx, by, totalW, totalH);
  }

#if X11_TRACE_PRESENT_ENABLED
  TS_FPRINTF("[BORDER] childXid=0x%08X parent=0x%08X bw=%d bp=0x%08X at=(%d,%d) total=%dx%d\n",
          (unsigned)childXid, (unsigned)cv.parent_xid,
          (int)bw, (unsigned)bp,
          (int)bx, (int)by, (int)totalW, (int)totalH);
#endif
}

static inline void sendInitialExposeNow(x11::XProtoContext& ctx, uint32_t wid) {
  if (wid == 0) return;

  x11::WindowView vw{};
  if (!ctx.windows().snapshot(wid, vw)) return;

  // Full-window expose.  Carry the sequence of the last processed request
  // (X11 spec: events use the sequence of the most recently processed request).
  const uint16_t seq = ctx.transport().lastSeq();

  auto ev = x11::wireev::buildExpose(seq,
                                     wid,
                                     /*x=*/0, /*y=*/0,
                                     vw.w, vw.h,
                                     /*count=*/0);

  (void)ctx.transport().sendEvent32(wid, ev.data());
}

// Erase a child window's pixels from the host surface after unmapping.
// In X11, unmapping a child removes it from the display.  Since our children
// share the host surface, we must repaint the child's area with the parent's
// background and re-expose any overlapping siblings.
static void eraseUnmappedChild(XProtoContext& ctx,
                               uint32_t childXid,
                               const WindowView& cv,
                               uint32_t host) {
  // Resolve the HOST surface directly (child is now unmapped, can't resolve it).
  DrawableRW hostDst{};
  if (!resolveDrawableRW(ctx, host, hostDst)) return;

  // Compute child's content origin in host surface coords.
  int32_t ox = 0, oy = 0;
  if (!ctx.windows().absoluteOffsetInHost(host, childXid, ox, oy)) return;

  // The child's total rect (border + content) in host surface coords.
  // absoluteOffsetInHost returns the CONTENT origin (x + bw accumulated).
  const int32_t bw = (int32_t)cv.border_width;
  int32_t rx = ox - bw;
  int32_t ry = oy - bw;
  int32_t rw = (int32_t)cv.w + 2*bw;
  int32_t rh = (int32_t)cv.h + 2*bw;

  // Clip to host surface bounds
  if (rx < 0) { rw += rx; rx = 0; }
  if (ry < 0) { rh += ry; ry = 0; }
  if (rx + rw > (int32_t)hostDst.w) rw = (int32_t)hostDst.w - rx;
  if (ry + rh > (int32_t)hostDst.h) rh = (int32_t)hostDst.h - ry;
  if (rw <= 0 || rh <= 0) return;

  // Fill with parent's background
  uint32_t parentBg = 0xFFFFFFFF; // default white
  ctx.windows().resolveBackgroundForClear(cv.parent_xid, parentBg);

  for (int32_t y = ry; y < ry + rh; y++) {
    uint32_t* row = hostDst.pixels32 + (size_t)y * hostDst.stridePixels;
    for (int32_t x = rx; x < rx + rw; x++) {
      row[x] = parentBg;
    }
  }

  // Damage the erased area
  damageOrDirty(ctx, host, rx, ry, rw, rh);

  // Re-expose mapped siblings that overlap the erased area.
  // Their border, background, and content will be repainted on top.
  auto siblings = ctx.windows().childrenInStackOrder(cv.parent_xid);
  for (uint32_t sib : siblings) {
    if (sib == childXid) continue;
    WindowView sv{};
    if (!ctx.windows().snapshot(sib, sv)) continue;
    if (!sv.mapped) continue;

    // Check overlap in parent coords
    const int32_t childX = (int32_t)cv.x;
    const int32_t childY = (int32_t)cv.y;
    const int32_t childTW = (int32_t)cv.w + 2*(int32_t)cv.border_width;
    const int32_t childTH = (int32_t)cv.h + 2*(int32_t)cv.border_width;

    const int32_t sibX = (int32_t)sv.x;
    const int32_t sibY = (int32_t)sv.y;
    const int32_t sibTW = (int32_t)sv.w + 2*(int32_t)sv.border_width;
    const int32_t sibTH = (int32_t)sv.h + 2*(int32_t)sv.border_width;

    if (sibX + sibTW <= childX || sibX >= childX + childTW) continue;
    if (sibY + sibTH <= childY || sibY >= childY + childTH) continue;

    // Sibling overlaps erased area — repaint border, background, send Expose
    fillWindowBorder(ctx, sib);
    fillWindowBackground(ctx, sib);
    sendInitialExposeNow(ctx, sib);
  }
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
  reg.registerMajor(x11::opcode::ChangeSaveSet,   &WindowOps::onMajor, this);  // 6
  reg.registerMajor(x11::opcode::CirculateWindow, &WindowOps::onMajor, this);  // 13
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
    case x11::opcode::ChangeSaveSet   : handleChangeSaveSet(ctx, dc.seq, dc.minor, dc.br); return;
    case x11::opcode::CirculateWindow : handleCirculateWindow(ctx, dc.seq, dc.minor, dc.br); return;
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
  uint32_t bg_pixmap = 0;          // CWBackPixmap > 1 (actual pixmap XID)
  uint32_t border_pixel_raw = 0;
  bool     has_border_pixel = false;
  // Window management attributes (Phase 4.3)
  uint8_t  bit_gravity = 0;      // CWBitGravity (bit 4): default Forget=0
  bool     has_bit_gravity = false;
  uint8_t  win_gravity = 1;      // CWWinGravity (bit 5): default NorthWest=1
  bool     has_win_gravity = false;
  uint8_t  backing_store = 0;    // CWBackingStore (bit 6): default NotUseful=0
  bool     has_backing_store = false;
  bool     override_redirect = false; // CWOverrideRedirect (bit 9)
  bool     has_override_redirect = false;
  uint32_t dnp_mask = 0;              // CWDontPropagate (bit 12)
  bool     has_dnp_mask = false;

  // Consume values for *all* bits set in increasing bit order.
  for (uint32_t bit = 0; bit < 32; bit++) {
    if (!(vmask & (1u << bit))) continue;
    if (br.remaining() < 4) break;
    const uint32_t val = br.readU32();
    switch (bit) {
      case 0: // CWBackPixmap
        if (val == 1) parent_relative = true;
        else if (val == 0) { /* None (no background, which is our default) */ }
        else { bg_pixmap = val; }  // actual pixmap XID
        break;
      case 1: // CWBackPixel
        if (val == 0)       bg_pixel = 0xFF000000u;       // black
        else if (val == 1)  bg_pixel = 0xFFFFFFFFu;       // white
        else                bg_pixel = 0xFF000000u | (val & 0x00FFFFFFu);
        has_bg_pixel = true;
        break;
      case 3: // CWBorderPixel
        border_pixel_raw = val;
        has_border_pixel = true;
        break;
      case 4: // CWBitGravity (0-10: Forget..Static)
        bit_gravity = (uint8_t)(val <= 10 ? val : 10);
        has_bit_gravity = true;
        break;
      case 5: // CWWinGravity (0-10: Unmap..Static)
        win_gravity = (uint8_t)(val <= 10 ? val : 10);
        has_win_gravity = true;
        break;
      case 6: // CWBackingStore (0-2: NotUseful/WhenMapped/Always)
        backing_store = (uint8_t)(val <= 2 ? val : 2);
        has_backing_store = true;
        break;
      case 9: // CWOverrideRedirect
        override_redirect = (val != 0);
        has_override_redirect = true;
        break;
      case 11: // CWEventMask
        event_mask = val;
        break;
      case 12: // CWDontPropagate (review 2026-08-31 §2.8: AWT fences off
               // ancestors with this on every window; ignoring it caused
               // duplicate/misattributed events)
        dnp_mask = val;
        has_dnp_mask = true;
        break;
      default:
        break; // consume but ignore unhandled bits (2, 7, 8, 10, 13-14)
    }
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
  // No size floor at CreateWindow time — store the client's requested size
  // exactly.  XQuartz/quartz-wm doesn't floor either; the WM reads
  // WM_NORMAL_HINTS at MapRequest time.  Our MAP_HINTS code in
  // handleMapWindow() does the equivalent: proactively reads hints at map
  // time and resizes.  Flooring here would change the client's perceived
  // geometry (via ConfigureNotify) and prevent it from sending its own
  // ConfigureWindow with the real size.
  const int owner_fd = ctx.transport().clientFd();
#if X11_TRACE_LIFECYCLE_ENABLED
  if (parent == x11::kRootXid) {
    TS_FPRINTF("[CREATE_TOPLEVEL] wid=0x%08X requested=%ux%u pos=(%d,%d) bw=%u\n",
            (unsigned)wid, (unsigned)wpx, (unsigned)hpx, (int)x, (int)y, (unsigned)borderWidth);
  }
#endif
  ctx.windows().upsert(wid, parent, x, y, wpx, hpx, event_mask, owner_fd);
#ifndef NDEBUG
  TS_FPRINTF("[GEOM] wid=0x%08X source=CREATE old=0x0 new=%ux%u xy=(%d,%d) parent=0x%08X or=%d\n",
          (unsigned)wid, (unsigned)wpx, (unsigned)hpx, (int)x, (int)y,
          (unsigned)parent, override_redirect ? 1 : 0);
#endif
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
  } else if (parent_relative) {
    // CWBackPixmap=ParentRelative: inherit the nearest ancestor's background_pixel.
    ctx.windows().resolveParentRelativeBackground(wid);
  } else if (bg_pixmap) {
    ctx.windows().setBackgroundPixmap(wid, bg_pixmap);
  }
  // Window management attributes (Phase 4.3)
  if (has_override_redirect) ctx.windows().setOverrideRedirect(wid, override_redirect);
  if (has_dnp_mask)          ctx.windows().setDontPropagateMask(wid, dnp_mask);
  if (has_win_gravity)       ctx.windows().setWinGravity(wid, win_gravity);
  if (has_bit_gravity)       ctx.windows().setBitGravity(wid, bit_gravity);
  if (has_backing_store)     ctx.windows().setBackingStore(wid, backing_store);


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

  uint32_t createFlags = 0;
  if (override_redirect) createFlags |= X11_UI_FLAG_OVERRIDE_REDIRECT;
  ctx.ui().pushCreate(wid, parent, title, (int32_t)x, (int32_t)y, (int32_t)wpx, (int32_t)hpx, createFlags);

  // Trace OR window creation for popup diagnosis
#ifndef NDEBUG
  if (override_redirect) {
    TS_FPRINTF("[CREATE_OR] wid=0x%08X parent=0x%08X pos=(%d,%d) size=%ux%u fd=%d\n",
            (unsigned)wid, (unsigned)parent, (int)x, (int)y,
            (unsigned)wpx, (unsigned)hpx, ctx.transport().clientFd());
  }
#endif

  // Optional: mark dirty so first present/expose happens when mapped/presentable.
  ctx.windows().markDirty(wid);

  // X11 spec: CreateNotify sent to parent if parent selects SubstructureNotifyMask
  {
    WindowView parentView{};
    if (parent == x11::kRootXid || ctx.windows().snapshot(parent, parentView)) {
      uint32_t parentMask = (parent == x11::kRootXid) ? 0 : parentView.event_mask;
      if (parentMask & x11::mask::SubstructureNotify) {
        auto ev = x11::wireev::buildCreateNotify(
          ctx.transport().lastSeq(),
          parent, wid, x, y, wpx, hpx, borderWidth, override_redirect);
        (void)ctx.transport().sendEvent32(parent, ev.data());
      }
    }
  }

#if X11_TRACE_LIFECYCLE_ENABLED
  TS_FPRINTF("[LIFECYCLE] CreateWindow wid=0x%08X parent=0x%08X xy=(%d,%d) wh=%ux%u evmask=0x%08X bg=%s\n",
          (unsigned)wid, (unsigned)parent,
          (int)x, (int)y, (unsigned)wpx, (unsigned)hpx,
          (unsigned)event_mask,
          has_bg_pixel ? "yes" : "no");
#endif

}


// -------------------- DestroyWindow
void WindowOps::handleDestroyWindow(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  // Request body: CARD32 window
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());

  if (wid == 0) return;

  // BadWindow if wid is not a known window
  if (!ctx.windows().exists(wid)) {
    ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::DestroyWindow);
    return;
  }

  // Clear WM-emulation state (pending map, peak size, post-map-notify)
  // so a recycled XID can't inherit ghost rescue state (§3.5).
  if (auto* srv = x11_proto_bridge_get_server()) {
    srv->clearWindowWmState(wid);
  }

  // Snapshot before erase for DestroyNotify delivery
  WindowView dv{};
  const bool hadSnap = ctx.windows().snapshot(wid, dv);
  const uint32_t parentXid = hadSnap ? dv.parent_xid : 0;

  // X11 spec: If this window has focus, send FocusOut and reset focus
  if (ctx.input().focus_xid == wid) {
    uint8_t fev[32] = {};
    fev[0]  = 10; // FocusOut
    fev[1]  = 0;  // detail = NotifyAncestor
    wire::wr16_le(fev + 2, ctx.transport().lastSeq());
    wire::wr32_le(fev + 4, wid);
    fev[8]  = 0;  // mode = NotifyNormal
    fev[9]  = 1;  // same-screen = true
    (void)ctx.transport().sendEvent32(wid, fev);
    applyFocusRevert(ctx, parentXid);
  }

  // Clear WM_TAKE_FOCUS bounce tracking if this window was in the history.
  // X11 window IDs get reused.  If a destroyed dialog's XID appears in the
  // bounce history, a new dialog that reuses that XID would be falsely
  // suppressed, preventing it from receiving focus (WM_TAKE_FOCUS not sent).
  {
    const uint32_t host = ctx.windows().topLevelAncestorOf(wid);
    const uint32_t check = host ? host : wid;
    if (ctx.input().take_focus_prev_ == check) ctx.input().take_focus_prev_ = 0;
    if (ctx.input().take_focus_last_ == check) ctx.input().take_focus_last_ = 0;
    if (ctx.input().focus_host == check) ctx.input().focus_host = 0;
  }

  // X11 spec: DestroyNotify sent to the window itself (StructureNotifyMask)
  // and to the parent (SubstructureNotifyMask)
  {
    const uint16_t evSeq = ctx.transport().lastSeq();
    if (hadSnap && (dv.event_mask & x11::mask::StructureNotify)) {
      auto ev = x11::wireev::buildDestroyNotify(evSeq, wid, wid);
      (void)ctx.transport().sendEvent32(wid, ev.data());
    }
    if (parentXid != 0 && parentXid != x11::kRootXid) {
      WindowView pv{};
      if (ctx.windows().snapshot(parentXid, pv) &&
          (pv.event_mask & x11::mask::SubstructureNotify)) {
        auto ev = x11::wireev::buildDestroyNotify(evSeq, parentXid, wid);
        (void)ctx.transport().sendEvent32(parentXid, ev.data());
      }
    }
  }

  // 1) Clear any grabs referencing this window (passive + active)
  ctx.grabs().removeForWindows({wid});

  // 2) Authoritative C++ state
  ctx.windows().erase(wid);

  // 3) Swift/UI teardown event path
  x11_ui_push_destroy(wid);
}

// -------------------- DestroySubwindows (opcode 5)
void WindowOps::handleDestroySubwindows(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());
  if (wid == 0) return;

  if (!ctx.windows().exists(wid)) {
    ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::DestroySubwindows);
    return;
  }

  // Get all descendants in BFS order, then destroy deepest first
  auto desc = ctx.windows().descendantsOf(wid);

  // Reverse: destroy deepest children first (leaf → root)
  for (auto it = desc.rbegin(); it != desc.rend(); ++it) {
    const uint32_t child = *it;

    // X11 spec: DestroyNotify to child (StructureNotifyMask)
    // and to its parent (SubstructureNotifyMask)
    {
      WindowView cv{};
      if (ctx.windows().snapshot(child, cv)) {
        const uint16_t evSeq = ctx.transport().lastSeq();
        if (cv.event_mask & x11::mask::StructureNotify) {
          auto ev = x11::wireev::buildDestroyNotify(evSeq, child, child);
          (void)ctx.transport().sendEvent32(child, ev.data());
        }
        const uint32_t parentXid = cv.parent_xid;
        if (parentXid != 0 && parentXid != x11::kRootXid) {
          WindowView pv{};
          if (ctx.windows().snapshot(parentXid, pv) &&
              (pv.event_mask & x11::mask::SubstructureNotify)) {
            auto ev = x11::wireev::buildDestroyNotify(evSeq, parentXid, child);
            (void)ctx.transport().sendEvent32(parentXid, ev.data());
          }
        }
      }
    }

    ctx.grabs().removeForWindows({child});
    ctx.windows().erase(child);
    x11_ui_push_destroy(child);
  }

#if X11_TRACE_LIFECYCLE_ENABLED
  TS_FPRINTF("[LIFECYCLE] DestroySubwindows parent=0x%08X destroyed=%zu children\n",
          (unsigned)wid, desc.size());
#endif
}

// -------------------- ReparentWindow (opcode 7)
void WindowOps::handleReparentWindow(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }
  const uint32_t wid       = br.readU32();
  const uint32_t newParent = br.readU32();
  const int16_t  x         = (int16_t)br.readU16();
  const int16_t  y         = (int16_t)br.readU16();
  br.skip(br.remaining());
  if (wid == 0) return;

  // Snapshot current state
  WindowView vw{};
  if (!ctx.windows().snapshot(wid, vw)) {
    ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::ReparentWindow);
    return;
  }
  const bool wasMapped = vw.mapped;

  // BadMatch if new parent is the window itself or a descendant of it
  if (newParent == wid) {
    ctx.transport().sendErrorCore(x11::error::BadMatch, seq, newParent, x11::opcode::ReparentWindow);
    return;
  }
  {
    auto desc = ctx.windows().descendantsOf(wid);
    for (uint32_t d : desc) {
      if (d == newParent) {
        ctx.transport().sendErrorCore(x11::error::BadMatch, seq, newParent, x11::opcode::ReparentWindow);
        return;
      }
    }
  }

  // If mapped, unmap first (X11 spec: ReparentWindow unmaps if mapped)
  if (wasMapped) {
    ctx.windows().setMapped(wid, false);
  }

  // Reparent in WindowTable
  ctx.windows().reparent(wid, newParent, x, y);

  // X11 spec: ReparentNotify sent to the window (StructureNotifyMask),
  // old parent (SubstructureNotifyMask), and new parent (SubstructureNotifyMask).
  {
    const uint16_t evSeq = ctx.transport().lastSeq();
    const uint32_t oldParent = vw.parent_xid;

    // Helper lambda to build a ReparentNotify event
    auto buildReparentNotify = [&](uint32_t eventWid) {
      uint8_t ev[32] = {};
      ev[0] = 21; // ReparentNotify
      wire::wr16_le(ev + 2, evSeq);
      wire::wr32_le(ev + 4, eventWid);    // event
      wire::wr32_le(ev + 8, wid);         // window
      wire::wr32_le(ev + 12, newParent);  // parent
      wire::wr16_le(ev + 16, (uint16_t)x);
      wire::wr16_le(ev + 18, (uint16_t)y);
      ev[20] = vw.override_redirect ? 1 : 0;
      (void)ctx.transport().sendEvent32(eventWid, ev);
    };

    // To the window itself (if StructureNotifyMask)
    if (vw.event_mask & x11::mask::StructureNotify) {
      buildReparentNotify(wid);
    }

    // To old parent (if SubstructureNotifyMask)
    if (oldParent != 0 && oldParent != x11::kRootXid) {
      WindowView opv{};
      if (ctx.windows().snapshot(oldParent, opv) &&
          (opv.event_mask & x11::mask::SubstructureNotify)) {
        buildReparentNotify(oldParent);
      }
    }

    // To new parent (if SubstructureNotifyMask)
    if (newParent != 0 && newParent != x11::kRootXid && newParent != oldParent) {
      WindowView npv{};
      if (ctx.windows().snapshot(newParent, npv) &&
          (npv.event_mask & x11::mask::SubstructureNotify)) {
        buildReparentNotify(newParent);
      }
    }
  }

  // If was mapped, remap
  if (wasMapped) {
    ctx.windows().setMapped(wid, true);
    fillWindowBorder(ctx, wid);
    fillWindowBackground(ctx, wid);
    sendInitialExposeNow(ctx, wid);
  }

#if X11_TRACE_LIFECYCLE_ENABLED
  TS_FPRINTF("[LIFECYCLE] ReparentWindow wid=0x%08X newParent=0x%08X xy=(%d,%d) wasMapped=%d\n",
          (unsigned)wid, (unsigned)newParent, (int)x, (int)y, (int)wasMapped);
#endif
}


// Helper: push resize/move/frame-extents after x11_ui_push_map().
// Used by both the immediate map path (normal-sized windows) and
// flushPendingMaps() in XProtoServer (deferred tiny windows).
static void pushMapExtras(XProtoContext& ctx, uint32_t wid) {
  WindowView vw{};
  if (!ctx.windows().snapshot(wid, vw)) return;

  x11_ui_push_resize(wid, (int32_t)vw.w, (int32_t)vw.h);

  if (vw.override_redirect) {
    x11_ui_push_move(wid, (int32_t)vw.x, (int32_t)vw.y);
  }

  // _NET_FRAME_EXTENTS: WM frame decoration sizes.
  {
    // All zeros — non-reparenting server, content-relative coordinates
    // (see XProtoServer.cpp counterpart for the top=28 history).
    uint8_t extents[16] = {0};
    PropertyTable::instance().setReplace(wid, x11::atom::k_NET_FRAME_EXTENTS,
                                         x11::atom::kCARDINAL, 32,
                                         extents, 16);
  }
}

  void WindowOps::handleMapWindow(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t wid = br.readU32();
    br.skip(br.remaining());
    if (wid == 0) return;

    if (!ctx.windows().exists(wid)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::MapWindow);
      return;
    }

    // 1) Update authoritative table
    // X11 spec: MapWindow on already-mapped window is a no-op.
    const bool wasMapped = ctx.windows().isMapped(wid);
    ctx.windows().setMapped(wid, true);

    // Rootless rule: only top-level host windows drive Cocoa.
    const uint32_t host = ctx.windows().topLevelAncestorOf(wid);

#if X11_TRACE_LIFECYCLE_ENABLED
    {
      WindowView mv{};
      ctx.windows().snapshot(wid, mv);
      TS_FPRINTF("[LIFECYCLE] MapWindow wid=0x%08X host=0x%08X isHost=%d parent=0x%08X xy=(%d,%d) wh=%ux%u wasMapped=%d\n",
              (unsigned)wid, (unsigned)host, (int)(host == wid),
              (unsigned)mv.parent_xid,
              (int)mv.x, (int)mv.y, (unsigned)mv.w, (unsigned)mv.h, (int)wasMapped);
    }
#endif

#if X11_TRACE_LIFECYCLE_ENABLED
    if (host != wid) {
      WindowView mv_dbg{};
      ctx.windows().snapshot(wid, mv_dbg);
      TS_FPRINTF("[LABEL] MapWindow child wid=0x%08X host=0x%08X parent=0x%08X pos=(%d,%d) size=%ux%u\n",
              (unsigned)wid, (unsigned)host,
              (unsigned)mv_dbg.parent_xid,
              (int)mv_dbg.x, (int)mv_dbg.y, (unsigned)mv_dbg.w, (unsigned)mv_dbg.h);
    }
#endif

    // X11 spec: MapNotify sent to the window (StructureNotifyMask) and
    // parent (SubstructureNotifyMask), but only if it wasn't already mapped.
    if (!wasMapped) {
      WindowView mv{};
      bool orFlag = false;
      if (ctx.windows().snapshot(wid, mv)) orFlag = mv.override_redirect;
      const uint16_t evSeq = ctx.transport().lastSeq();
      // To window itself
      if (mv.event_mask & x11::mask::StructureNotify) {
        auto ev = x11::wireev::buildMapNotify(evSeq, wid, wid, orFlag);
        (void)ctx.transport().sendEvent32(wid, ev.data());
      }
      // To parent
      if (mv.parent_xid != 0 && mv.parent_xid != x11::kRootXid) {
        WindowView pv{};
        if (ctx.windows().snapshot(mv.parent_xid, pv) &&
            (pv.event_mask & x11::mask::SubstructureNotify)) {
          auto ev = x11::wireev::buildMapNotify(evSeq, mv.parent_xid, wid, orFlag);
          (void)ctx.transport().sendEvent32(mv.parent_xid, ev.data());
        }
      }
    }

    // Already-mapped window: skip UI push (prevents double MAP_SHOW when
    // MapSubwindows already pushed a map for this host).
    if (wasMapped) goto post_map;

    // 2) Swift-side map + authoritative resize only for the host (UI command queue)
    {
      bool deferred = false;
      if (host == wid) {

        // ── SubstructureRedirect emulation: defer map for tiny windows ────
        // Clients like Java AWT create windows at 1×1, then configure them to
        // the real size.  Without SubstructureRedirect, MapWindow fires before
        // ConfigureWindow arrives.  Deferring the map until all buffered client
        // data is drained allows ConfigureWindow + WM_NORMAL_HINTS to land
        // first.  The daemon's poll loop calls flushPendingMaps() after
        // readAndDispatch returns NeedMore (all buffered data consumed).
        //
        // Also defer when the client has shrunk the window below its own
        // earlier peak ConfigureWindow size (Vivado/AWT race: a stale
        // ConfigureWindow arrives AFTER WM_NORMAL_HINTS committed the real
        // size, walking the window back to its initial skeleton dimensions).
        //
        // **override_redirect EXEMPTION** (v1.19.35.57, fixes hw_ila drag +
        // xterm Ctrl+V paste regressions):  Real X11 servers MUST NOT redirect
        // MapRequest on override_redirect windows.  OR=1 means "WM/server,
        // hands off — this is a private helper window".  Java AWT's XDND
        // drag-and-drop creates 1×1 off-screen OR helper windows; xterm and
        // other toolkits create similar OR proxies for selection transfer.
        // Our SubstructureRedirect emulation was rescuing those helpers to a
        // 500×300 fallback and moving them on-screen, which broke the client's
        // own protocol logic the moment it tried GetProperty/ChangeProperty
        // on its newly-created window.  Skip the deferral entirely for OR.
        WindowView pre_map_vw{};
        bool is_tiny = false;
        bool shrunk_below_peak = false;
        bool is_or = false;
        if (ctx.windows().snapshot(wid, pre_map_vw)) {
          is_or   = pre_map_vw.override_redirect;
          is_tiny = (pre_map_vw.w < 50 || pre_map_vw.h < 50);
          if (!is_or && !is_tiny) {
            uint16_t pw = 0, ph = 0;
            if (ctx.getPeakSize(wid, pw, ph)) {
              const uint32_t cur_area  = (uint32_t)pre_map_vw.w * pre_map_vw.h;
              const uint32_t peak_area = (uint32_t)pw * ph;
              // Peak is more than 2× current area: client shrank its own win.
              if (peak_area > cur_area * 2) {
                shrunk_below_peak = true;
#ifndef NDEBUG
                TS_FPRINTF("[GEOM] wid=0x%08X source=MAP_SHRUNK_BELOW_PEAK cur=%ux%u peak=%ux%u — deferring\n",
                        (unsigned)wid, (unsigned)pre_map_vw.w, (unsigned)pre_map_vw.h,
                        (unsigned)pw, (unsigned)ph);
#endif
              }
            }
          }
        }

        if (!is_or && (is_tiny || shrunk_below_peak)) {
#if X11_TRACE_LIFECYCLE_ENABLED
          TS_FPRINTF("[MAP_DEFER] wid=0x%08X geom=%ux%u — deferring map (reason=%s)\n",
                  (unsigned)wid, (unsigned)pre_map_vw.w, (unsigned)pre_map_vw.h,
                  is_tiny ? "tiny" : "shrunk_below_peak");
#endif
          ctx.addPendingMap(wid);
          deferred = true;
        } else {
          x11_ui_push_map(wid);
          pushMapExtras(ctx, wid);
        }
      }

      // For deferred maps, skip the post_map block.  Sending Expose and
      // queueing notify at the TINY pre-flush size would cause the client to
      // draw at 1×1 before the window is resized to its real dimensions.
      // flushPendingMaps() will resize the window and push the map to Swift;
      // SetPresentable (after surface registration at real size) will fill
      // the background and call sendExposeSubtree at the correct geometry.
      if (deferred) return;
    }

    post_map:
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


    // 6) If anything drew before presentable, flush once now (route to host).
    //    Skip for override-redirect windows: the surface doesn't exist yet at
    //    MapWindow time (Swift hasn't created the NSWindow/MTKView), so this
    //    damage push triggers a premature present with blank (white) content.
    //    The SetPresentable handler (after surface registration) will push
    //    damage that triggers the first real present with actual client content.
    if (host != 0) {
      x11::WindowView mv2{};
      if (ctx.windows().snapshot(host, mv2) && !mv2.override_redirect) {
        x11_shared_damage_union(host, 0, 0, (int32_t)mv2.w, (int32_t)mv2.h);
        x11_ui_push_damage(host, 0, 0, (int32_t)mv2.w, (int32_t)mv2.h);
      }
    }
  }
  
  
void WindowOps::handleMapSubwindows(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t parent = br.readU32();
  br.skip(br.remaining());
  if (parent == 0) return;

  if (!ctx.windows().exists(parent)) {
    ctx.transport().sendErrorCore(x11::error::BadWindow, seq, parent, x11::opcode::MapSubwindows);
    return;
  }

  // Rootless host for this subtree.
  const uint32_t host = ctx.windows().topLevelAncestorOf(parent);

  // X11 spec: "equivalent to performing a MapWindow request on each unmapped child."
  // Only direct children — NOT all descendants. Descendants' individual map states
  // are preserved; they become viewable when their parent chain is all mapped.
  auto desc = ctx.windows().childrenInStackOrder(parent);

#if X11_TRACE_LIFECYCLE_ENABLED
  TS_FPRINTF("[LIFECYCLE] MapSubwindows parent=0x%08X host=0x%08X numChildren=%zu\n",
          (unsigned)parent, (unsigned)host, desc.size());
  for (uint32_t xid : desc) {
    WindowView dv{};
    bool ok = ctx.windows().snapshot(xid, dv);
    TS_FPRINTF("[LIFECYCLE]   child=0x%08X mapped=%d parent=0x%08X xy=(%d,%d) wh=%ux%u\n",
            (unsigned)xid, ok ? (int)dv.mapped : -1,
            ok ? (unsigned)dv.parent_xid : 0u,
            ok ? (int)dv.x : 0, ok ? (int)dv.y : 0,
            ok ? (unsigned)dv.w : 0u, ok ? (unsigned)dv.h : 0u);
  }
#endif

#if X11_TRACE_LIFECYCLE_ENABLED
  // Detailed hierarchy dump for windows with many children (like xcalc's Form).
  // Shows stacking order, which helps identify misplaced/overlapping children.
  if (desc.size() >= 10) {
    auto siblings = ctx.windows().childrenInStackOrder(parent);
    TS_FPRINTF("[HIERARCHY] parent=0x%08X numChildren=%zu (bottom-to-top stacking):\n",
            (unsigned)parent, siblings.size());
    int stackIdx = 0;
    for (uint32_t sib : siblings) {
      WindowView sv{};
      bool ok = ctx.windows().snapshot(sib, sv);
      TS_FPRINTF("[HIERARCHY]   [%2d] xid=0x%08X xy=(%4d,%4d) wh=%4ux%4u bw=%u bg=%s\n",
              stackIdx++, (unsigned)sib,
              ok ? (int)sv.x : 0, ok ? (int)sv.y : 0,
              ok ? (unsigned)sv.w : 0u, ok ? (unsigned)sv.h : 0u,
              ok ? (unsigned)sv.border_width : 0u,
              (ok && sv.has_background_pixel) ? "yes" : "no");
    }
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

    // X11 spec: MapNotify to the window and parent
    {
      WindowView childView{};
      if (ctx.windows().snapshot(xid, childView)) {
        const uint16_t evSeq = ctx.transport().lastSeq();
        if (childView.event_mask & x11::mask::StructureNotify) {
          auto ev = x11::wireev::buildMapNotify(evSeq, xid, xid, childView.override_redirect);
          (void)ctx.transport().sendEvent32(xid, ev.data());
        }
        if (parent != 0 && parent != x11::kRootXid) {
          WindowView pv{};
          if (ctx.windows().snapshot(parent, pv) &&
              (pv.event_mask & x11::mask::SubstructureNotify)) {
            auto ev = x11::wireev::buildMapNotify(evSeq, parent, xid, childView.override_redirect);
            (void)ctx.transport().sendEvent32(parent, ev.data());
          }
        }
      }
    }

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
  
  
void WindowOps::handleUnmapWindow(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());
  if (wid == 0) return;

  if (!ctx.windows().exists(wid)) {
    ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::UnmapWindow);
    return;
  }

  // Clear WM-emulation state: a withdrawn dialog legitimately
  // reconfigured smaller must not trip the stale-peak "shrunk-below-peak"
  // rescue on re-map (§3.5).
  if (auto* srv = x11_proto_bridge_get_server()) {
    srv->clearWindowWmState(wid);
  }

  // Snapshot geometry BEFORE unmapping (geometry persists, but we need wasMapped).
  WindowView cv{};
  const bool hadSnap = ctx.windows().snapshot(wid, cv);
  const bool wasMapped = hadSnap && cv.mapped;

  // X11 spec: UnmapNotify sent to window (StructureNotifyMask) and
  // parent (SubstructureNotifyMask), but only if it was mapped.
  if (wasMapped) {
    const uint16_t evSeq = ctx.transport().lastSeq();
    if (hadSnap && (cv.event_mask & x11::mask::StructureNotify)) {
      auto ev = x11::wireev::buildUnmapNotify(evSeq, wid, wid, /*fromConfigure*/false);
      (void)ctx.transport().sendEvent32(wid, ev.data());
    }
    if (cv.parent_xid != 0 && cv.parent_xid != x11::kRootXid) {
      WindowView pv{};
      if (ctx.windows().snapshot(cv.parent_xid, pv) &&
          (pv.event_mask & x11::mask::SubstructureNotify)) {
        auto ev = x11::wireev::buildUnmapNotify(evSeq, cv.parent_xid, wid, /*fromConfigure*/false);
        (void)ctx.transport().sendEvent32(cv.parent_xid, ev.data());
      }
    }
  }

  // X11 spec: If unmapped window has focus, send FocusOut and reset focus
  if (wasMapped && ctx.input().focus_xid == wid) {
    uint8_t fev[32] = {};
    fev[0]  = 10; // FocusOut
    fev[1]  = 0;  // detail = NotifyAncestor
    wire::wr16_le(fev + 2, ctx.transport().lastSeq());
    wire::wr32_le(fev + 4, wid);
    fev[8]  = 0;  // mode = NotifyNormal
    fev[9]  = 1;  // same-screen = true
    (void)ctx.transport().sendEvent32(wid, fev);
    applyFocusRevert(ctx, cv.parent_xid);
  }

  // 1) Update authoritative table
  ctx.windows().setMapped(wid, false);

  // 2) Rootless: route "needs repaint later" to host, not child.
  const uint32_t host = ctx.windows().topLevelAncestorOf(wid);
  if (host != 0) {
    ctx.windows().markDirty(host);
  } else {
    ctx.windows().markDirty(wid);
  }

  // 3) Erase the child's pixels from the host surface.
  //    In X11, unmapping a child removes it from the display.  Since our
  //    children share the host surface, we must repaint the child's area
  //    with the parent's background and re-expose overlapping siblings.
  if (wasMapped && host != 0 && host != wid) {
    eraseUnmappedChild(ctx, wid, cv, host);
  }

  // 4) Swift/UI visibility: only top-level host should actually be hidden.
  if (host == wid) {
    x11_ui_push_unmap(wid);
  }
}
  
// -----------------------------
// UnmapSubwindows (major 11)
// -----------------------------
void WindowOps::handleUnmapSubwindows(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  // Request body: CARD32 parent
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t parent = br.readU32();
  br.skip(br.remaining());
  if (parent == 0) return;

  if (!ctx.windows().exists(parent)) {
    ctx.transport().sendErrorCore(x11::error::BadWindow, seq, parent, x11::opcode::UnmapSubwindows);
    return;
  }

  // X11 spec: "equivalent to performing an UnmapWindow request on each mapped child."
  // Only direct children — NOT all descendants. Descendants keep their individual
  // map state but become not-viewable when their parent is unmapped.
  auto desc = ctx.windows().childrenInStackOrder(parent);

  for (uint32_t xid : desc) {
    WindowView cv{};
    if (!ctx.windows().snapshot(xid, cv)) continue;
    if (!cv.mapped) continue; // already unmapped

    // X11 spec: UnmapNotify to the window and parent
    {
      const uint16_t evSeq = ctx.transport().lastSeq();
      if (cv.event_mask & x11::mask::StructureNotify) {
        auto ev = x11::wireev::buildUnmapNotify(evSeq, xid, xid, /*fromConfigure*/false);
        (void)ctx.transport().sendEvent32(xid, ev.data());
      }
      if (parent != 0 && parent != x11::kRootXid) {
        WindowView pv{};
        if (ctx.windows().snapshot(parent, pv) &&
            (pv.event_mask & x11::mask::SubstructureNotify)) {
          auto ev = x11::wireev::buildUnmapNotify(evSeq, parent, xid, /*fromConfigure*/false);
          (void)ctx.transport().sendEvent32(parent, ev.data());
        }
      }
    }

    // 1) Authoritative state
    ctx.windows().setMapped(xid, false);

    // 2) Rootless: ensure the host will repaint to reflect the child disappearing
    const uint32_t host = ctx.windows().topLevelAncestorOf(xid);
    if (host != 0) {
      ctx.windows().markDirty(host);

      // 3) Erase child’s pixels from host surface
      if (host != xid) {
        eraseUnmappedChild(ctx, xid, cv, host);
      }
    }
  }

  // If the parent itself is a top-level host, you may optionally mark it dirty too
  const uint32_t hostP = ctx.windows().topLevelAncestorOf(parent);
  if (hostP != 0) ctx.windows().markDirty(hostP);
}
  
  
// -------------------- ChangeSaveSet (opcode 6) --------------------------
// mode (dc.minor): 0=Insert, 1=Delete
// Save-set is only meaningful for reparenting window managers.
// In rootless mode, this is a safe no-op.
void WindowOps::handleChangeSaveSet(XProtoContext& /*ctx*/, uint16_t /*seq*/,
                                     uint8_t /*mode*/, ByteReader& br) {
  br.skip(br.remaining());
}

// -------------------- CirculateWindow (opcode 13)
// dc.minor = direction: 0=RaiseLowest, 1=LowerHighest
void WindowOps::handleCirculateWindow(XProtoContext& ctx, uint16_t seq,
                                       uint8_t direction, ByteReader& br)
{
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  br.skip(br.remaining());
  if (wid == 0) return;

  if (!ctx.windows().exists(wid)) {
    ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::CirculateWindow);
    return;
  }

  // Get children in stacking order
  auto children = ctx.windows().childrenInStackOrder(wid);
  if (children.size() < 2) return; // nothing to circulate

  uint32_t target = 0;
  uint8_t place = 0; // 0=Top, 1=Bottom

  if (direction == 0) {
    // RaiseLowest: find lowest mapped child and raise it to the top
    for (uint32_t child : children) {
      WindowView cv{};
      if (ctx.windows().snapshot(child, cv) && cv.mapped) {
        target = child;
        place = 0; // Top
        break;
      }
    }
    if (target != 0) ctx.windows().raiseToTop(target);
  } else {
    // LowerHighest: find highest mapped child and lower it to the bottom
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
      WindowView cv{};
      if (ctx.windows().snapshot(*it, cv) && cv.mapped) {
        target = *it;
        place = 1; // Bottom
        break;
      }
    }
    if (target != 0) ctx.windows().lowerToBottom(target);
  }

  if (target == 0) return; // no mapped children

  // X11 spec: CirculateNotify sent to the window (StructureNotifyMask)
  // and to the parent (SubstructureNotifyMask)
  {
    const uint16_t evSeq = ctx.transport().lastSeq();

    // To the parent (wid) if it selects StructureNotify
    WindowView parentView{};
    if (ctx.windows().snapshot(wid, parentView) &&
        (parentView.event_mask & x11::mask::StructureNotify)) {
      auto ev = x11::wireev::buildCirculateNotify(evSeq, wid, target, place);
      (void)ctx.transport().sendEvent32(wid, ev.data());
    }

    // To the circulated child if it selects StructureNotify
    WindowView childView{};
    if (ctx.windows().snapshot(target, childView) &&
        (childView.event_mask & x11::mask::StructureNotify)) {
      auto ev = x11::wireev::buildCirculateNotify(evSeq, target, target, place);
      (void)ctx.transport().sendEvent32(target, ev.data());
    }
  }

  // Damage host for repaint
  const uint32_t host = ctx.windows().topLevelAncestorOf(wid);
  if (host != 0) {
    damageOrDirty(ctx, host);
  }
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
#ifndef NDEBUG
  TS_FPRINTF("[GEOM] wid=0x%08X source=COCOA_RESIZE old=%ux%u new=%ux%u\n",
          (unsigned)wid, (unsigned)old_w, (unsigned)old_h,
          (unsigned)new_w, (unsigned)new_h);
#endif
  
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
