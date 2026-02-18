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

// Bridge -- Update C-side 
#include "XProtoServerBridge.h"

extern "C" {
#include "x11_requests.h"
#include "x11_backend_fb.h"
}

// util
#include "Damage.hpp"
#include "Debug/x11_backend_fb_dbg.hpp"



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
  void WindowAttrOps::handleChangeWindowAttributes(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
    ctx.tracef("[CWA] ENTER remain=%zu\n", br.remaining());
    fprintf(stderr, "[CWA] ENTER remain=%zu\n", br.remaining());
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }

    const uint32_t wid   = br.readU32();
    const uint32_t vmask = br.readU32();

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

    // Value list is 32-bit items in increasing bit order.
    // We must consume every provided value in order, even if we ignore most.
    for (uint32_t bit = 0; bit < 32 && br.remaining() >= 4; bit++) {
      if ((vmask & (1u << bit)) == 0) continue;
      const uint32_t val = br.readU32();

      switch (bit) {
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
      // You need WindowState::cursor_xid + WindowTable::setCursor as discussed.
      ctx.windows().setCursor(wid, newCursor);
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
  
  
  void WindowAttrOps::handleConfigureWindow(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
    // Body (after 4-byte header):
    //   CARD32 window
    //   CARD16 valueMask
    //   CARD16 pad
    //   LISTofCARD32 values  (in increasing bit order of valueMask)
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }

    const uint32_t wid   = br.readU32();
    const uint16_t vmask = br.readU16();
    (void)br.readU16(); // pad

    // Compute host once (rootless policy pivot).
    const uint32_t host = ctx.windows().topLevelAncestorOf(wid);

    // Pull current values (so “partial configure” keeps the rest)
    int32_t  x32 = 0, y32 = 0;
    uint32_t w32 = 1, h32 = 1;

    // Keep old size around so we can decide whether to resize the FB.
    // (This avoids extra churn, and makes the intent explicit.)
    uint16_t oldW = 1, oldH = 1;

    if (const WindowView* vw = ctx.window(wid)) {
      x32  = vw->x;
      y32  = vw->y;
      w32  = vw->w ? vw->w : 1;
      h32  = vw->h ? vw->h : 1;
      oldW = vw->w ? vw->w : 1;
      oldH = vw->h ? vw->h : 1;
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

    // Track whether this request actually supplies Width/Height.
    // (We only resize FBs when the request is truly a size change request.)
    bool sawWidth  = false;
    bool sawHeight = false;

    // Values are 32-bit units in bit order 0..15
    for (uint32_t bit = 0; bit < 16; bit++) {
      if ((vmask & (uint16_t)(1u << bit)) == 0) continue;
      if (br.remaining() < 4) break;

      const uint32_t v = br.readU32();

      switch (bit) {
        case 0: x32 = (int32_t)v; break; // X
        case 1: y32 = (int32_t)v; break; // Y
        case 2: w32 = v; sawWidth  = true; break; // Width
        case 3: h32 = v; sawHeight = true; break; // Height
        default:
          // Ignore others (stacking/border/etc) for now.
          break;
      }
    }

    // Consume any trailing bytes (defensive)
    br.skip(br.remaining());

    const int16_t  x = clamp_i16(x32);
    const int16_t  y = clamp_i16(y32);
    const uint16_t w = clamp_u16_nonzero(w32);
    const uint16_t h = clamp_u16_nonzero(h32);

    ctx.tracef("[CONFIGURE] wid=0x%08X x=%d y=%d w=%u h=%u host=0x%08X\n",
               wid, (int)x, (int)y, (unsigned)w, (unsigned)h, host);

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
    // 2) Rootless policy:
    //
    //    - Only HOST owns the Cocoa NSWindow (top-level surface).
    //    - Child ConfigureWindow must NOT resize the host framebuffer.
    //
    //    HOWEVER:
    //    - In host-only present mode, the C compositor still composites mapped
    //      descendants by blitting each child's *own* FB over the host.
    //    - Therefore, whenever a child window's geometry changes in W/H, we MUST
    //      keep that child's FB slot in sync with its WindowTable geometry.
    //
    //    This is the invariant:
    //      for mapped windows: geom(w,h) == fb(w,h)
    // ------------------------------------------------------------------
    const bool sizeChanged = (w != oldW) || (h != oldH);
    const bool requestTouchesSize = (sawWidth || sawHeight); // request included W/H fields

    if (host != 0 && host == wid) {
      // HOST: keep C canonical FB in sync for the host only.
      FB_RESIZE(wid, w, h, "handleConfigureWindow(host)");

      // Host geometry change affects presentation.
      damageOrDirty(ctx, wid);

      // Only the top-level host drives Cocoa resize.
      x11_requests_push_configure(wid, (int32_t)w, (int32_t)h);

    } else if (host != 0) {
      // CHILD:
      // - Must NOT resize the host framebuffer.
      // - Must resize the CHILD FB when W/H is configured, otherwise the compositor
      //   sees geom(new) with fb(old) and you get the "top-left only / stripe" artifacts.
      //
      // We only do this when the request actually carries W/H (ConfigureWindow can be
      // position-only, stacking-only, etc).
      if (requestTouchesSize && sizeChanged) {
        FB_RESIZE(wid, w, h, "handleConfigureWindow(child)");
      }

      // Child geometry changed: it affects what the host should present.
      // Trigger present pipeline on the host (gated).
      damageOrDirty(ctx, host);
    }

    // ------------------------------------------------------------------
    // 3) ConfigureNotify / ExposeNotify selection on THE WINDOW BEING CONFIGURED
    // ------------------------------------------------------------------
    if (const WindowView* vw2 = ctx.window(wid)) {
      const bool wantCfg = ((vw2->event_mask & (1u << 17)) != 0); // StructureNotifyMask
      const bool wantExp = ((vw2->event_mask & (1u << 15)) != 0); // ExposureMask
      if (wantCfg || wantExp) {
        ctx.transport().queueNotify(wid, wantCfg, wantExp);
      }
    }
  }
  
  void WindowAttrOps::handleGetWindowAttributes(XProtoContext& ctx, uint16_t seq, ByteReader& br)
  {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t wid = br.readU32();
    br.skip(br.remaining());

    // Prefer WindowTable snapshot
    const WindowView* wv = ctx.window(wid);

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
    
    // If drawable is a known window, return its geometry.
    if (const WindowView* vw = ctx.window(drawable)) {
      x = vw->x;
      y = vw->y;
      w = vw->w;
      h = vw->h;
    }
    
    // Use ReplyWriter helper (already in your code)
    (void)ctx.reply().sendGetGeometryReply(seq, root, x, y, w, h, border, kDepth);
  }
  
  
} // namespace x11
