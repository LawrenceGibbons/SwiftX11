//
//  WindowAttrOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/26/26.
//

#include "Ops/WindowAttrOps.hpp"

#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include "Utils/ByteReader.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Core/XConstants.hpp"
#include "Utils/WireLE.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/CursorRouting.hpp"
#include "Core/InputRouting.hpp"
#include "Core/DrawableRW.hpp"
#include "Core/PixmapTable.hpp"
#include "Utils/WireEvents.hpp"

// Bridge -- Update C-side 
#include "XProtoServerBridge.h"

extern "C" {
#include "SwiftX11Bridge.h"
}

// util
#include "Damage.hpp"



static constexpr uint32_t kRootVis   = 0x00000021u; // X11_ROOT_VIS
static constexpr uint32_t kRootCmap  = 0x00000020u; // defaultColormap advertised

namespace x11 {

WindowAttrOps::WindowAttrOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::ChangeWindowAttributes, &WindowAttrOps::onMajor, this); // ChangeWindowAttributes
  reg.registerMajor(x11::opcode::GetWindowAttributes   , &WindowAttrOps::onMajor, this);
  reg.registerMajor(x11::opcode::ConfigureWindow,        &WindowAttrOps::onMajor, this);  // ConfigureWindow
  reg.registerMajor(x11::opcode::GetGeometry,            &WindowAttrOps::onMajor, this);  // GetGeometry

}

void WindowAttrOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<WindowAttrOps*>(user)->handle(ctx, dc);
}

void WindowAttrOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::ChangeWindowAttributes: handleChangeWindowAttributes(ctx, dc.seq, dc.br); return;
    case x11::opcode::GetWindowAttributes   : handleGetWindowAttributes(ctx, dc.seq, dc.br); return;
    case x11::opcode::ConfigureWindow       : handleConfigureWindow(ctx, dc.seq, dc.br); return;
    case x11::opcode::GetGeometry           : handleGetGeometry(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[WindowAttrOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// Major=2 request body (after 4-byte header):
//   CARD32 window
//   CARD32 valueMask
//   LISTofCARD32 valueList
//
// We only implement CWEventMask (bit 11) for now.
  void WindowAttrOps::handleChangeWindowAttributes(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    ctx.tracef("[CWA] ENTER remain=%zu\n", br.remaining());
#ifdef X11_TRACE_VERBOSE
    fprintf(stderr, "[CWA] ENTER remain=%zu\n", br.remaining());
#endif
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }

    const uint32_t wid   = br.readU32();
    const uint32_t vmask = br.readU32();

    ctx.tracef("[CWA] wid=0x%08X vmask=0x%08X\n", wid, vmask);

    // BadWindow error for unknown window XIDs (allow None=0 and root=1 through)
    if (!ctx.window(wid) && wid > 1) {
      br.skip(br.remaining());
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid,
                                    x11::opcode::ChangeWindowAttributes);
      return;
    }

    // Snapshot current values so partial updates preserve unspecified fields.
    uint32_t old_mask = 0;
    bool wasMapped = false;
    if (const WindowView* vw = ctx.window(wid)) {
      old_mask = vw->event_mask;
      wasMapped = vw->mapped;
    }

    ctx.tracef("[CWA] GATE1 wid=0x%08X vmask=0x%08X remain=%zu old_mask=0x%08X mapped=%d\n",
               wid, vmask, br.remaining(), old_mask, (int)wasMapped);

    uint32_t cur_mask = old_mask;
    bool sawEventMask = false;

    uint32_t newCursor = 0;
    bool sawCursor = false;

    uint32_t newBgPixel = 0;
    bool sawBgPixel = false;

    bool sawBackPixmap = false;
    bool parentRelative = false;
    bool backPixmapNone = false;

    uint32_t newBorderPixel = 0;
    bool sawBorderPixel = false;

    // Value list is 32-bit items in increasing bit order.
    // We must consume every provided value in order, even if we ignore most.
    for (uint32_t bit = 0; bit < 32 && br.remaining() >= 4; bit++) {
      if ((vmask & (1u << bit)) == 0) continue;
      const uint32_t val = br.readU32();

      switch (bit) {
        case 0: // CWBackPixmap
          sawBackPixmap = true;
          if (val == 1) {
            parentRelative = true;
          } else if (val == 0) {
            backPixmapNone = true;
          }
          // val > 1 → pixmap ID (not supported yet)
          ctx.tracef("[CWA] CWBackPixmap wid=0x%08X val=0x%08X parentRel=%d none=%d\n",
                     wid, val, (int)parentRelative, (int)backPixmapNone);
          break;

        case 1: // CWBackPixel
          // Map X11 pixel value to ARGB8888 (force alpha opaque)
          if (val == 0)       newBgPixel = 0xFF000000u;       // black
          else if (val == 1)  newBgPixel = 0xFFFFFFFFu;       // white
          else                newBgPixel = 0xFF000000u | (val & 0x00FFFFFFu);
          sawBgPixel = true;
          ctx.tracef("[CWA] CWBackPixel wid=0x%08X val=0x%08X → argb=0x%08X\n",
                     wid, val, newBgPixel);
          break;

        case 3: // CWBorderPixel
          if (val == 0)       newBorderPixel = 0xFF000000u;
          else if (val == 1)  newBorderPixel = 0xFFFFFFFFu;
          else                newBorderPixel = 0xFF000000u | (val & 0x00FFFFFFu);
          sawBorderPixel = true;
          ctx.tracef("[CWA] CWBorderPixel wid=0x%08X val=0x%08X → argb=0x%08X\n",
                     wid, val, newBorderPixel);
          break;

        case 11: // CWEventMask
          cur_mask = val;
          sawEventMask = true;
          ctx.tracef("[CWA] CWEventMask wid=0x%08X val=0x%08X\n", wid, cur_mask);
          break;

        case 14: // CWCursor
          newCursor = val;  // 0 is valid (None / inherit)
          sawCursor = true;
          ctx.tracef("[CWA] CWCursor wid=0x%08X cursor=0x%08X\n", wid, newCursor);
          break;

        default:
          break;
      }
    }

    // Consume any trailing bytes
    br.skip(br.remaining());

    // ---- Apply cursor even if event mask wasn't present ----
    if (sawCursor) {
      ctx.windows().setCursor(wid, newCursor);

      const uint32_t host = ctx.windows().topLevelAncestorOf(wid);

      // Only touch the Cocoa cursor for this host if the pointer is actually in it.
      const bool pointerInThisHost = (host != 0) && (ctx.input().last_xid == host || ctx.input().focus_host == host);
      if (pointerInThisHost) {
        uint32_t underNow = x11::pickDeepestMappedWindowAtHostPoint(ctx, host,
                                                                    ctx.input().win_x_u,
                                                                    ctx.input().win_y_u);
        if (!underNow) underNow = host;

        const uint32_t cursorTarget = ctx.input().routePointer(underNow);
        maybeApplyCursor(ctx, host, cursorTarget);
      }
    }
    
    // ---- Apply background pixmap/pixel ----
    // Per X11 spec: CWBackPixmap takes precedence over CWBackPixel when both specified.
    // Handle ParentRelative first, then let explicit CWBackPixel override if both present.
    if (sawBackPixmap && !sawBgPixel) {
      if (parentRelative) {
        ctx.windows().resolveParentRelativeBackground(wid);
      } else if (backPixmapNone) {
        ctx.windows().clearBackground(wid);
      }
    }
    if (sawBgPixel) {
      ctx.windows().setBackgroundPixel(wid, newBgPixel);
    }

    // ---- Apply border pixel if present ----
    if (sawBorderPixel) {
      ctx.windows().setBorderPixel(wid, newBorderPixel);
    }

    // ---- Apply event mask only if present ----
    if (!sawEventMask) return;

    ctx.windows().setEventMask(wid, cur_mask);

    // Exposure “unstick” (optional; only meaningful when we changed event_mask)
    const bool hadExposure = ((old_mask & (1u << 15)) != 0);
    const bool wantExpose  = ((cur_mask & (1u << 15)) != 0);
    if (wasMapped && !hadExposure && wantExpose) {
      ctx.tracef("[CWA] ExposureMask enabled post-map; queue initial Expose wid=0x%08X\n", wid);
      ctx.transport().queueNotify(wid, /*wantCfg=*/false, /*wantExp=*/true);
    }
  }
  
  
  void WindowAttrOps::handleConfigureWindow(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    // Body (after 4-byte header):
    //   CARD32 window
    //   CARD16 valueMask
    //   CARD16 pad
    //   LISTofCARD32 values  (in increasing bit order of valueMask)
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }

    const uint32_t wid   = br.readU32();
    const uint16_t vmask = br.readU16();
    (void)br.readU16(); // pad

    // BadWindow error for unknown window XIDs (allow None=0 and root=1 through)
    if (!ctx.window(wid) && wid > 1) {
      br.skip(br.remaining());
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid,
                                    x11::opcode::ConfigureWindow);
      return;
    }

    // Compute host once (rootless policy pivot).
    const uint32_t host = ctx.windows().topLevelAncestorOf(wid);

    // Pull current values (so “partial configure” keeps the rest)
    int32_t  x32 = 0, y32 = 0;
    uint32_t w32 = 1, h32 = 1;
    uint16_t borderW = 0;

    if (const WindowView* vw = ctx.window(wid)) {
      x32  = vw->x;
      y32  = vw->y;
      w32  = vw->w ? vw->w : 1;
      h32  = vw->h ? vw->h : 1;
      borderW = vw->border_width;
    }

    auto clamp_i16 = [](int32_t v) -> int16_t {
      if (v < (int32_t)INT16_MIN) return INT16_MIN;
      if (v > (int32_t)INT16_MAX) return INT16_MAX;
      return (int16_t)v;
    };
    auto clamp_u16_nonzero = [](uint32_t v) -> uint16_t {
      if (v == 0) return 1;
      if (v > 65535u) return 65535u;
      return (uint16_t)v;
    };

    // CWSibling + CWStackMode (bits 5-6)
    uint32_t sibling = 0;
    uint8_t  stackMode = 0;  // Above=0, Below=1, TopIf=2, BottomIf=3, Opposite=4
    bool     hasSibling   = false;
    bool     hasStackMode = false;

    // Values are 32-bit units in bit order 0..15
    for (uint32_t bit = 0; bit < 16; bit++) {
      if ((vmask & (uint16_t)(1u << bit)) == 0) continue;
      if (br.remaining() < 4) break;

      const uint32_t v = br.readU32();

      switch (bit) {
        case 0: x32 = (int32_t)v; break;               // CWX
        case 1: y32 = (int32_t)v; break;               // CWY
        case 2: w32 = v; break;                         // CWWidth
        case 3: h32 = v; break;                         // CWHeight
        case 4: borderW = (uint16_t)(v & 0xFFFF); break; // CWBorderWidth
        case 5: sibling = v; hasSibling = true; break;  // CWSibling
        case 6: stackMode = (uint8_t)(v & 0xFF);        // CWStackMode
                hasStackMode = true; break;
        default:
          break;
      }
    }

    // Consume any trailing bytes (defensive)
    br.skip(br.remaining());

    const int16_t  x = clamp_i16(x32);
    const int16_t  y = clamp_i16(y32);
    const uint16_t w = clamp_u16_nonzero(w32);
    const uint16_t h = clamp_u16_nonzero(h32);

    ctx.tracef("[CONFIGURE] wid=0x%08X x=%d y=%d w=%u h=%u bw=%u host=0x%08X\n",
               wid, (int)x, (int)y, (unsigned)w, (unsigned)h, (unsigned)borderW, host);

    // Apply border width if changed
    if (vmask & (1u << 4)) {
      ctx.windows().setBorderWidth(wid, borderW);
    }

    // ------------------------------------------------------------------
    // 0.5) Stacking order (CWStackMode, optionally with CWSibling)
    // ------------------------------------------------------------------
    if (hasStackMode) {
      switch (stackMode) {
        case 0: // Above
          if (hasSibling)
            ctx.windows().restackAbove(wid, sibling);
          else
            ctx.windows().raiseToTop(wid);
          break;
        case 1: // Below
          if (hasSibling)
            ctx.windows().restackBelow(wid, sibling);
          else
            ctx.windows().lowerToBottom(wid);
          break;
        case 2: // TopIf — raise if any sibling obscures it
          ctx.windows().raiseToTop(wid);
          break;
        case 3: // BottomIf — lower if it obscures any sibling
          ctx.windows().lowerToBottom(wid);
          break;
        case 4: // Opposite — toggle
          ctx.windows().raiseToTop(wid);
          break;
        default:
          break;
      }
#ifndef NDEBUG
      fprintf(stderr, "[RESTACK] wid=0x%08X stackMode=%u sibling=0x%08X\n",
              wid, (unsigned)stackMode, sibling);
#endif
    }

    // ------------------------------------------------------------------
    // 1) Update WindowTable geometry (X11 semantics)
    // ------------------------------------------------------------------
    //
    // NOTE: For host windows, we apply rootless clamp policy (descendants clamped)
    // via setGeometryRootlessHost(). For children, we apply the geometry directly.
    //
    if (host != 0 && host == wid) {
      // Host geometry update should clamp descendants (bring-up behavior).
      ctx.windows().setGeometryRootlessHost(wid, x, y, w, h);
    } else {
      // Prefer the debug-tagged setter if available so logs show "ConfigureWindow"
      // instead of a generic "setGeometry".
      //
      // If your WindowTable doesn't expose setGeometryDbg, replace this with setGeometry().
      ctx.windows().setGeometryDbg(wid, x, y, w, h, "ConfigureWindow", __FILE__, __LINE__);
    }

    // ------------------------------------------------------------------
    // 2) Rootless policy — Swift owns all backing surfaces.
    //
    //    - Only HOST owns the Cocoa NSWindow (top-level surface).
    //    - Child windows draw into the host surface at their offset.
    //    - No C framebuffers exist; surface allocation is Swift's job.
    // ------------------------------------------------------------------

    if (host != 0 && host == wid) {
      // HOST: geometry change affects presentation. Full window repaint.
      damageOrDirty(ctx, wid, 0, 0, (int32_t)w, (int32_t)h);

      // Only the top-level host drives Cocoa resize.
      x11_ui_push_resize(wid, (int32_t)w, (int32_t)h);

    } else if (host != 0) {
      // CHILD: geometry changed — it affects what the host should present.
      // Full host repaint (child moved/resized within host surface).
      damageOrDirty(ctx, host);

      // NOTE: We intentionally do NOT fill the child's background here.
      // During live resize, the BG fill wipes the child's area with its
      // background_pixel colour. A present then fires before the client
      // redraws, producing a blank frame (e.g., scrollbar disappears).
      // The client will paint the correct content in response to the
      // Expose event we send below.  Omitting the fill means the surface
      // may show stale pixels for one frame — acceptable and far less
      // jarring than a blank/white flash.
    }

    // ------------------------------------------------------------------
    // 3) ConfigureNotify + Expose for THE WINDOW BEING CONFIGURED.
    //    Send Expose directly (not just queued) to ensure the child
    //    redraws promptly after being repositioned.
    // ------------------------------------------------------------------
    if (const WindowView* vw2 = ctx.window(wid)) {
      const bool wantCfg = ((vw2->event_mask & (1u << 17)) != 0); // StructureNotifyMask
      if (wantCfg) {
        ctx.transport().queueNotify(wid, /*wantConfigure=*/true, /*wantExpose=*/false);
      }
      // Always send a direct Expose (bypasses notify queue coalescing).
      // This ensures the child repaints at its new position immediately.
      auto ev = x11::wireev::buildExpose(ctx.transport().lastSeq(),
                                         wid, 0, 0, vw2->w, vw2->h, 0);
      ctx.transport().sendEvent32(wid, ev.data());
    }
  }
  
  void WindowAttrOps::handleGetWindowAttributes(XProtoContext& ctx, uint16_t seq, ByteReader& br)
  {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t wid = br.readU32();
    br.skip(br.remaining());

    // Prefer WindowTable snapshot
    const WindowView* wv = ctx.window(wid);

    // BadWindow error for unknown window XIDs (allow None=0 and root=1 through)
    if (!wv && wid > 1) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid,
                                    x11::opcode::GetWindowAttributes);
      return;
    }

    const uint32_t eventMask = wv ? wv->event_mask : 0;
    const uint8_t  mapState  = (wv && wv->mapped) ? 2 : 0; // Viewable=2, Unmapped=0

    // Reply payload is 44 bytes total. That means:
    // - 32-byte reply header
    // - 12 bytes extra payload (3 * 4-byte units)
    //
    // Your C implementation builds all 44 bytes as a single buffer; we’ll do the same.
    std::array<uint8_t, 44> rep{};
    rep.fill(0);

    rep[0] = 1;   // Reply
    rep[1] = 0;   // backing-store = NotUseful

    // seq
    wire::wr16_le(rep.data() + 2, seq);

    // length_words = (44-32)/4 = 3
    wire::wr32_le(rep.data() + 4, 3);

    // visual
    wire::wr32_le(rep.data() + 8, kRootVis);

    // class = InputOutput (CARD16)
    wire::wr16_le(rep.data() + 12, 1);

    // bit-gravity / win-gravity
    rep[14] = 0; // Forget
    rep[15] = 0; // Unmap

    // backing-planes / backing-pixel
    wire::wr32_le(rep.data() + 16, 0);
    wire::wr32_le(rep.data() + 20, 0);

    // save-under / map-is-installed / map-state / override-redirect
    rep[24] = 0;        // saveUnder
    rep[25] = 1;        // mapIsInstalled (true)
    rep[26] = mapState; // mapState
    rep[27] = 0;        // overrideRedirect

    // colormap
    wire::wr32_le(rep.data() + 28, kRootCmap);

    // all-event-masks / your-event-mask
    wire::wr32_le(rep.data() + 32, eventMask);
    wire::wr32_le(rep.data() + 36, eventMask);

    // do-not-propagate-mask + pad
    wire::wr16_le(rep.data() + 40, 0);
    wire::wr16_le(rep.data() + 42, 0);

    // IMPORTANT: this is a single 44-byte reply (not “32 + payload separately”)
    // so just sendReplyBytes directly.
    (void)ctx.transport().sendReplyBytes(rep.data(), rep.size());
  }
  
  
  
  // ---- 14: GetGeometry ----
  void WindowAttrOps::handleGetGeometry(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    // Request: CARD32 drawable
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t drawable = br.readU32();
    br.skip(br.remaining());

    uint32_t root = kRootXid;
    int16_t  x = 0, y = 0;
    uint16_t w = kRootW, h = kRootH;
    uint16_t border = 0;
    uint16_t depth = kDepth;

    // If drawable is a known window, return its geometry.
    if (const WindowView* vw = ctx.window(drawable)) {
      x = vw->x;
      y = vw->y;
      w = vw->w;
      h = vw->h;
      border = vw->border_width;
    } else {
      // Try pixmap lookup
      PixmapView pv;
      if (ctx.pixmaps().snapshot(drawable, pv)) {
        x = 0;
        y = 0;
        w = pv.w;
        h = pv.h;
        border = 0;
        depth = pv.depth;
      } else if (drawable > 1) {
        // Not a window, not a pixmap, not root/None — BadDrawable
        ctx.transport().sendErrorCore(x11::error::BadDrawable, seq, drawable,
                                      x11::opcode::GetGeometry);
        return;
      }
    }

    // Use ReplyWriter helper
    (void)ctx.reply().sendGetGeometryReply(seq, root, x, y, w, h, border, depth);
  }
  
  
} // namespace x11
