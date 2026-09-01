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
#include "Core/ScreenLayout.hpp"
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
#include "Utils/MachTime.hpp"



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
    TS_FPRINTF("[CWA] ENTER remain=%zu\n", br.remaining());
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
    uint32_t backPixmapId = 0;

    uint32_t newBorderPixel = 0;
    bool sawBorderPixel = false;

    // Window management attributes (Phase 4.3)
    uint8_t newBitGravity = 0;    bool sawBitGravity = false;
    uint8_t newWinGravity = 0;    bool sawWinGravity = false;
    uint8_t newBackingStore = 0;  bool sawBackingStore = false;
    bool    newOverrideRedirect = false; bool sawOverrideRedirect = false;

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
          } else {
            backPixmapId = val;  // actual pixmap XID
          }
          ctx.tracef("[CWA] CWBackPixmap wid=0x%08X val=0x%08X parentRel=%d none=%d pix=0x%X\n",
                     wid, val, (int)parentRelative, (int)backPixmapNone, backPixmapId);
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

        case 4: // CWBitGravity (0-10: Forget..Static)
          newBitGravity = (uint8_t)(val <= 10 ? val : 10);
          sawBitGravity = true;
          break;

        case 5: // CWWinGravity (0-10: Unmap..Static)
          newWinGravity = (uint8_t)(val <= 10 ? val : 10);
          sawWinGravity = true;
          break;

        case 6: // CWBackingStore (0-2: NotUseful/WhenMapped/Always)
          newBackingStore = (uint8_t)(val <= 2 ? val : 2);
          sawBackingStore = true;
          break;

        case 9: // CWOverrideRedirect
          newOverrideRedirect = (val != 0);
          sawOverrideRedirect = true;
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
          break; // consume but ignore unhandled bits (2, 7, 8, 10, 12, 13)
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
      } else if (backPixmapId) {
        ctx.windows().setBackgroundPixmap(wid, backPixmapId);
      }
    }
    if (sawBgPixel) {
      ctx.windows().setBackgroundPixel(wid, newBgPixel);
    }

    // ---- Apply border pixel if present ----
    if (sawBorderPixel) {
      ctx.windows().setBorderPixel(wid, newBorderPixel);
    }

    // ---- Apply window management attributes (Phase 4.3) ----
    if (sawBitGravity)       ctx.windows().setBitGravity(wid, newBitGravity);
    if (sawWinGravity)       ctx.windows().setWinGravity(wid, newWinGravity);
    if (sawBackingStore)     ctx.windows().setBackingStore(wid, newBackingStore);
    if (sawOverrideRedirect) ctx.windows().setOverrideRedirect(wid, newOverrideRedirect);

    // ---- Apply event mask only if present ----
    if (!sawEventMask) return;

    ctx.windows().setEventMask(wid, cur_mask);

    // Exposure "unstick" (optional; only meaningful when we changed event_mask)
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

#ifndef NDEBUG
    {
      const WindowView* dbg_vw = ctx.window(wid);
      if (dbg_vw && dbg_vw->override_redirect) {
        TS_FPRINTF("[CONFIGURE_OR] wid=0x%08X vmask=0x%04X cur_pos=(%d,%d) cur_size=%ux%u bits=%s%s%s%s%s%s%s\n",
                (unsigned)wid, (unsigned)vmask,
                (int)dbg_vw->x, (int)dbg_vw->y,
                (unsigned)dbg_vw->w, (unsigned)dbg_vw->h,
                (vmask & 0x01) ? "X " : "",
                (vmask & 0x02) ? "Y " : "",
                (vmask & 0x04) ? "W " : "",
                (vmask & 0x08) ? "H " : "",
                (vmask & 0x10) ? "BW " : "",
                (vmask & 0x20) ? "SIB " : "",
                (vmask & 0x40) ? "STACK " : "");
      }
    }
#endif

    // Pull current values (so "partial configure" keeps the rest)
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

    // BadValue if client explicitly sets width or height to 0
    if ((vmask & (1u << 2)) && w32 == 0) {
      ctx.transport().sendErrorCore(x11::error::BadValue, seq, 0, x11::opcode::ConfigureWindow);
      return;
    }
    if ((vmask & (1u << 3)) && h32 == 0) {
      ctx.transport().sendErrorCore(x11::error::BadValue, seq, 0, x11::opcode::ConfigureWindow);
      return;
    }

    const int16_t  x = clamp_i16(x32);
    const int16_t  y = clamp_i16(y32);
    uint16_t w = clamp_u16_nonzero(w32);
    uint16_t h = clamp_u16_nonzero(h32);

    ctx.tracef("[CONFIGURE] wid=0x%08X x=%d y=%d w=%u h=%u bw=%u host=0x%08X\n",
               wid, (int)x, (int)y, (unsigned)w, (unsigned)h, (unsigned)borderW, host);
#ifndef NDEBUG
    if (host != 0 && host == wid) {
      TS_FPRINTF("[CONFIGURE_TOPLEVEL] wid=0x%08X vmask=0x%X w=%u h=%u pos=(%d,%d)\n",
              (unsigned)wid, (unsigned)vmask, (unsigned)w, (unsigned)h, (int)x, (int)y);
    }
#endif

    // Apply border width if changed
    if (vmask & (1u << 4)) {
      ctx.windows().setBorderWidth(wid, borderW);
    }

    // ------------------------------------------------------------------
    // 0.5) Stacking order (CWStackMode, optionally with CWSibling)
    // ------------------------------------------------------------------
    if (hasStackMode) {
      // Validate sibling shares same parent as wid (BadMatch if not)
      if (hasSibling && sibling != 0) {
        WindowView widView{}, sibView{};
        if (ctx.windows().snapshot(wid, widView) && ctx.windows().snapshot(sibling, sibView)) {
          if (widView.parent_xid != sibView.parent_xid) {
            ctx.transport().sendErrorCore(x11::error::BadMatch, seq, sibling, x11::opcode::ConfigureWindow);
            return;
          }
        }
      }
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
    }

    // ------------------------------------------------------------------
    // 1) Update WindowTable geometry (X11 semantics)
    // ------------------------------------------------------------------
    //
    // NOTE: For host windows, we apply rootless clamp policy (descendants clamped)
    // via setGeometryRootlessHost(). For children, we apply the geometry directly.
    //
#ifndef NDEBUG
    uint16_t old_w_geom = 0, old_h_geom = 0;
    int16_t  old_x_geom = 0, old_y_geom = 0;
    if (const WindowView* pv = ctx.window(wid)) {
      old_w_geom = pv->w; old_h_geom = pv->h;
      old_x_geom = pv->x; old_y_geom = pv->y;
    }
#endif
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
#ifndef NDEBUG
    TS_FPRINTF("[GEOM] wid=0x%08X source=CONFIG vmask=0x%04X old=%ux%u@(%d,%d) new=%ux%u@(%d,%d) mapped=%d host=%d\n",
            (unsigned)wid, (unsigned)vmask,
            (unsigned)old_w_geom, (unsigned)old_h_geom, (int)old_x_geom, (int)old_y_geom,
            (unsigned)w, (unsigned)h, (int)x, (int)y,
            ctx.windows().isMapped(wid) ? 1 : 0,
            (host == wid) ? 1 : 0);
#endif

    // ------------------------------------------------------------------
    // 1.5) Peak pre-map size tracking (SubstructureRedirect emulation)
    // ------------------------------------------------------------------
    // Track the largest ConfigureWindow size for root children.
    // If the client undoes its resize (e.g., Java AWT ConfigureWindow→1×1)
    // before MapWindow, flushPendingMaps uses the peak size instead.
    // The unmapped/pending gate lives server-side in notePeakSize —
    // windows in pending_maps_ are already marked mapped, and the old
    // call-site isMapped() check went blind for exactly those (§3.4).
    if (host != 0 && host == wid && (vmask & 0x0C)) {
      ctx.notePeakSize(wid, w, h);
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

      // For override-redirect windows (popup menus, tooltips), push
      // position to Swift so the NSWindow moves to the correct screen
      // location. Normal windows are positioned by Cocoa.
      if (vmask & 0x03) { // CWX (bit 0) or CWY (bit 1) changed
        const WindowView* vw3 = ctx.window(wid);
        if (vw3 && vw3->override_redirect) {
          x11_ui_push_move(wid, (int32_t)x, (int32_t)y);
        }
      }

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

      // X11 spec: ConfigureNotify also sent to parent with SubstructureNotifyMask
      if (vw2->parent_xid != 0 && vw2->parent_xid != x11::kRootXid) {
        WindowView pv{};
        if (ctx.windows().snapshot(vw2->parent_xid, pv) &&
            (pv.event_mask & x11::mask::SubstructureNotify)) {
          // Find above-sibling (window just below this one in stacking order)
          uint32_t aboveSib = 0;
          {
            auto sibs = ctx.windows().childrenInStackOrder(vw2->parent_xid);
            for (size_t i = 1; i < sibs.size(); i++) {
              if (sibs[i] == wid) { aboveSib = sibs[i-1]; break; }
            }
          }
          auto cfgEv = x11::wireev::buildConfigureNotify(
            ctx.transport().lastSeq(),
            vw2->parent_xid, wid, aboveSib,
            vw2->x, vw2->y, vw2->w, vw2->h,
            vw2->border_width, vw2->override_redirect);
          (void)ctx.transport().sendEvent32(vw2->parent_xid, cfgEv.data());
        }
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

    // Retrieve stored window management attributes
    const uint8_t backingStore     = wv ? wv->backing_store : 0;
    const uint8_t bitGravity       = wv ? wv->bit_gravity : 0;
    const uint8_t winGravity       = wv ? wv->win_gravity : 1;
    const bool    overrideRedirect = wv ? wv->override_redirect : false;

    rep[0] = 1;               // Reply
    rep[1] = backingStore;    // backing-store (0=NotUseful, 1=WhenMapped, 2=Always)

    // seq
    wire::wr16_le(rep.data() + 2, seq);

    // length_words = (44-32)/4 = 3
    wire::wr32_le(rep.data() + 4, 3);

    // visual
    wire::wr32_le(rep.data() + 8, kRootVis);

    // class = InputOutput (CARD16)
    wire::wr16_le(rep.data() + 12, 1);

    // bit-gravity / win-gravity
    rep[14] = bitGravity;
    rep[15] = winGravity;

    // backing-planes / backing-pixel
    wire::wr32_le(rep.data() + 16, 0);
    wire::wr32_le(rep.data() + 20, 0);

    // save-under / map-is-installed / map-state / override-redirect
    rep[24] = 0;        // saveUnder
    rep[25] = 1;        // mapIsInstalled (true)
    rep[26] = mapState; // mapState
    rep[27] = overrideRedirect ? 1 : 0;

    // colormap
    wire::wr32_le(rep.data() + 28, kRootCmap);

    // all-event-masks / your-event-mask
    wire::wr32_le(rep.data() + 32, eventMask);
    wire::wr32_le(rep.data() + 36, eventMask);

    // do-not-propagate-mask + pad
    wire::wr16_le(rep.data() + 40, 0);
    wire::wr16_le(rep.data() + 42, 0);

    // IMPORTANT: this is a single 44-byte reply (not "32 + payload separately")
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
    // Default to actual virtual desktop dimensions (for root window queries)
    const auto screenLayout = x11::getScreenLayout();
    uint16_t w = screenLayout.virtual_w;
    uint16_t h = screenLayout.virtual_h;
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
