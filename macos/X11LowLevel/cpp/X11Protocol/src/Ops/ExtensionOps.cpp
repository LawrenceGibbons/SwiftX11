//
//  ExtensionOps.cpp
//  X11LowLevel
//
//  Minimal extension stubs so that clients (GTK, Java AWT, Qt) get valid
//  version-query replies instead of sequence-desync crashes.
//
//  Each extension registers its assigned major opcode.  The handler
//  dispatches on dc.minor (the sub-opcode inside the extension).
//  Minor 0 is conventionally the version / query request.
//

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "SwiftX11Bridge.h"
}

#include "Ops/ExtensionOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/WindowTable.hpp"
#include "Core/WindowView.hpp"
#include "Core/PixmapTable.hpp"
#include "Core/ShapeRegion.hpp"
#include "Core/ScreenLayout.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Core/XClient.hpp"
#include "Utils/ByteReader.hpp"
#include "Utils/WireLE.hpp"
#include "Utils/WireErrors.hpp"
#include "Core/X11ExtOpcodes.hpp"
#include "Core/InputState.hpp"

// Bridge function (defined in UICommandQueue.cpp)
extern "C" void x11_ui_push_shape_changed(uint32_t host_xid);

namespace x11 {

// ============================================================================
// Registration
// ============================================================================
ExtensionOps::ExtensionOps(XProtoRegistrar& reg) {
  reg.registerMajor(ext::kXFIXES,    &ExtensionOps::onMajor, this);
  reg.registerMajor(ext::kSHAPE,     &ExtensionOps::onMajor, this);
  reg.registerMajor(ext::kRANDR,     &ExtensionOps::onMajor, this);
  reg.registerMajor(ext::kXinerama,  &ExtensionOps::onMajor, this);
  reg.registerMajor(ext::kGE,        &ExtensionOps::onMajor, this);
  reg.registerMajor(ext::kXCMisc,    &ExtensionOps::onMajor, this);
  reg.registerMajor(ext::kXInput2,   &ExtensionOps::onMajor, this);
  reg.registerMajor(ext::kXTEST,     &ExtensionOps::onMajor, this);
  reg.registerMajor(ext::kCOMPOSITE, &ExtensionOps::onMajor, this);
  reg.registerMajor(ext::kDAMAGE,    &ExtensionOps::onMajor, this);
}

void ExtensionOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<ExtensionOps*>(user)->handle(ctx, dc);
}


// ============================================================================
// Dispatch
// ============================================================================
void ExtensionOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  const uint8_t major = dc.major;
  const uint8_t minor = dc.minor;
  const uint16_t seq  = dc.seq;
  ByteReader& br      = dc.br;

  // -------------------------------------------------------------------
  // XFIXES — minor 0 = QueryVersion, plus minimal sub-opcodes
  // -------------------------------------------------------------------
  if (major == ext::kXFIXES) {
    switch (minor) {
    case 0: {
      // XFixesQueryVersion request: CARD32 client_major, CARD32 client_minor
      br.skip(br.remaining());
      // Reply: major=5, minor=0
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0); // length
        wire::wr32_le(rep.data() + 8, 5); // server major version
        wire::wr32_le(rep.data() + 12, 0); // server minor version
      });
      return;
    }
    case 1: // XFixesChangeSaveSet — extended save-set with map/target modes (void)
      br.skip(br.remaining());
      return;
    case 2: // XFixesSelectSelectionInput — selection change event mask (void)
      br.skip(br.remaining());
      return;
    case 3: // SelectCursorInput — consume silently (cursor event selection)
      br.skip(br.remaining());
      return;
    case 4: {
      // GetCursorImage — reply with 1x1 transparent cursor
      br.skip(br.remaining());
      // Reply: 32-byte header + 4 bytes of cursor pixel data
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 1); // length = 1 word (4 bytes of pixel data)
        wire::wr16_le(rep.data() + 8, 0);  // x
        wire::wr16_le(rep.data() + 10, 0); // y
        wire::wr16_le(rep.data() + 12, 1); // width
        wire::wr16_le(rep.data() + 14, 1); // height
        wire::wr16_le(rep.data() + 16, 0); // xhot
        wire::wr16_le(rep.data() + 18, 0); // yhot
        wire::wr32_le(rep.data() + 20, 0); // cursor_serial
      });
      // 1x1 transparent pixel
      uint8_t pixel[4] = {0, 0, 0, 0};
      ctx.reply().sendBytes(pixel, 4);
      return;
    }
    case 5: // CreateRegion — track XID existence silently
    case 6: // CreateRegionFromBitmap
    case 7: // CreateRegionFromWindow
    case 8: // CreateRegionFromGC
    case 9: // CreateRegionFromPicture
      br.skip(br.remaining());
      return;
    case 10: // DestroyRegion — consume silently
    case 11: // SetRegion
    case 12: // CopyRegion
    case 13: // UnionRegion
    case 14: // IntersectRegion
    case 15: // SubtractRegion
    case 16: // InvertRegion
    case 17: // TranslateRegion
    case 18: // RegionExtents
      br.skip(br.remaining());
      return;
    case 19: {
      // FetchRegion — reply with empty region
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0); // length = 0 (no rectangles)
        // extents: x1=0, y1=0, x2=0, y2=0
      });
      return;
    }
    case 20: // SetGCClipRegion
    case 21: // SetWindowShapeRegion
    case 22: // SetPictureClipRegion
      br.skip(br.remaining());
      return;
    case 23: // SetCursorName
    case 24: // GetCursorName — would need reply
      if (minor == 24) {
        br.skip(br.remaining());
        // Reply with empty name
        (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
          wire::wr32_le(rep.data() + 4, 0); // length
          wire::wr32_le(rep.data() + 8, 0); // atom = None
          wire::wr16_le(rep.data() + 12, 0); // nbytes = 0
        });
        return;
      }
      br.skip(br.remaining());
      return;
    case 25: // GetCursorImageAndName
    {
      br.skip(br.remaining());
      // Reply: 1x1 cursor with empty name
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 1); // length = 1 word (pixel data)
        wire::wr16_le(rep.data() + 8, 0);  // x
        wire::wr16_le(rep.data() + 10, 0); // y
        wire::wr16_le(rep.data() + 12, 1); // width
        wire::wr16_le(rep.data() + 14, 1); // height
        wire::wr16_le(rep.data() + 16, 0); // xhot
        wire::wr16_le(rep.data() + 18, 0); // yhot
        wire::wr32_le(rep.data() + 20, 0); // cursor_serial
        wire::wr32_le(rep.data() + 24, 0); // cursor_atom
        wire::wr16_le(rep.data() + 28, 0); // nbytes
      });
      uint8_t pixel[4] = {0, 0, 0, 0};
      ctx.reply().sendBytes(pixel, 4);
      return;
    }
    case 26: // ChangeCursor
    case 27: // ChangeCursorByName
      br.skip(br.remaining());
      return;
    case 29: // HideCursor — consume silently
    case 30: // ShowCursor — consume silently
      br.skip(br.remaining());
      return;
    case 31: // CreatePointerBarrier
    case 32: // DeletePointerBarrier
      br.skip(br.remaining());
      return;
    default:
      { char buf[128]; snprintf(buf, sizeof(buf), "[XFIXES] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
      br.skip(br.remaining());
      // Send error to prevent XCB sequence desync if sub-opcode was reply-bearing.
      ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
      return;
    }
  }

  // -------------------------------------------------------------------
  // SHAPE — minor 0 = QueryVersion, plus minimal sub-opcodes
  // -------------------------------------------------------------------
  if (major == ext::kSHAPE) {
    switch (minor) {
    case 0: {
      // ShapeQueryVersion — no request body
      br.skip(br.remaining());
      // Reply: major=1, minor=1
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0); // length
        wire::wr16_le(rep.data() + 8, 1); // major version
        wire::wr16_le(rep.data() + 10, 1); // minor version
      });
      return;
    }
    case 1: {
      // ShapeRectangles
      // Body after 4-byte header: op(1B), kind(1B), ordering(2B),
      //   window(4B), x_off(2B), y_off(2B), rects...
      if (br.remaining() < 12) { br.skip(br.remaining()); return; }
      const uint8_t  op   = br.readU8();   // Set=0, Union=1, Intersect=2, Subtract=3, Invert=4
      const uint8_t  kind = br.readU8();   // Bounding=0, Clip=1, Input=2
      br.skip(2); // ordering
      const uint32_t wid  = br.readU32();
      const int16_t  x_off = (int16_t)br.readU16();
      const int16_t  y_off = (int16_t)br.readU16();

      // Parse rectangle list
      const size_t nrects = br.remaining() / 8;
      std::vector<ShapeRegion::Rect> shapeRects;
      shapeRects.reserve(nrects);
      for (size_t i = 0; i < nrects; i++) {
        ShapeRegion::Rect r;
        r.x = (int16_t)br.readU16();
        r.y = (int16_t)br.readU16();
        r.w = br.readU16();
        r.h = br.readU16();
        shapeRects.push_back(r);
      }
      br.skip(br.remaining());

      // Get window dimensions for shape ops
      WindowView vw{};
      uint16_t ww = 1, wh = 1;
      if (ctx.windows().snapshot(wid, vw)) { ww = vw.w; wh = vw.h; }

      // Build shape region (get existing for non-Set ops)
      ShapeRegion region;
      if (op != 0) { // non-Set ops need existing region
        if (kind == 0)      region = ctx.windows().shapeBounding(wid);
        else if (kind == 1) region = ctx.windows().shapeBounding(wid);
        else if (kind == 2) region = ctx.windows().shapeInput(wid);
      }
      region.setFromRects(shapeRects.data(), shapeRects.size(), x_off, y_off, ww, wh, op);

      // Store
      if (kind == 0)      ctx.windows().setShapeBounding(wid, std::move(region));
      else if (kind == 1) ctx.windows().setShapeClip(wid, std::move(region));
      else if (kind == 2) ctx.windows().setShapeInput(wid, std::move(region));

      // Notify Swift that shape changed (for visual clipping)
      uint32_t host = ctx.windows().topLevelAncestorOf(wid);
      if (host == 0) host = wid;
      x11_ui_push_shape_changed(host);

      return;
    }
    case 2: {
      // ShapeMask
      // Body: op(1B), kind(1B), unused(2B), window(4B), x_off(2B), y_off(2B), source_bitmap(4B)
      if (br.remaining() < 16) { br.skip(br.remaining()); return; }
      const uint8_t  op   = br.readU8();
      const uint8_t  kind = br.readU8();
      br.skip(2); // unused
      const uint32_t wid  = br.readU32();
      const int16_t  x_off = (int16_t)br.readU16();
      const int16_t  y_off = (int16_t)br.readU16();
      const uint32_t pixmap_id = br.readU32();
      br.skip(br.remaining());

      WindowView vw{};
      uint16_t ww = 1, wh = 1;
      if (ctx.windows().snapshot(wid, vw)) { ww = vw.w; wh = vw.h; }

      ShapeRegion region;
      if (pixmap_id == 0) {
        // None — reset to unshaped
        region.reset();
      } else {
        // Look up depth-1 pixmap
        PixmapView pm{};
        bool found = ctx.pixmaps().snapshot(pixmap_id, pm);
        if (found && pm.depth == 1 && pm.bits) {
          if (op != 0) {
            if (kind == 0)      region = ctx.windows().shapeBounding(wid);
            else if (kind == 2) region = ctx.windows().shapeInput(wid);
          }
          region.setFromBitmap(pm.bits, pm.w, pm.h, (int)pm.stride_bytes,
                               x_off, y_off, ww, wh, op);
        } else {
          // Unknown pixmap — treat as unshaped
          region.reset();
        }
      }

      if (kind == 0)      ctx.windows().setShapeBounding(wid, std::move(region));
      else if (kind == 1) ctx.windows().setShapeClip(wid, std::move(region));
      else if (kind == 2) ctx.windows().setShapeInput(wid, std::move(region));

      uint32_t host = ctx.windows().topLevelAncestorOf(wid);
      if (host == 0) host = wid;
      x11_ui_push_shape_changed(host);

      return;
    }
    case 3: {
      // ShapeCombine
      // Body: op(1B), destKind(1B), srcKind(1B), unused(1B),
      //       dest(4B), x_off(2B), y_off(2B), src(4B)
      if (br.remaining() < 16) { br.skip(br.remaining()); return; }
      const uint8_t  op       = br.readU8();
      const uint8_t  destKind = br.readU8();
      const uint8_t  srcKind  = br.readU8();
      br.skip(1); // unused
      const uint32_t destWid  = br.readU32();
      const int16_t  x_off    = (int16_t)br.readU16();
      const int16_t  y_off    = (int16_t)br.readU16();
      const uint32_t srcWid   = br.readU32();
      br.skip(br.remaining());

      WindowView vw{};
      uint16_t ww = 1, wh = 1;
      if (ctx.windows().snapshot(destWid, vw)) { ww = vw.w; wh = vw.h; }

      // Get source shape
      ShapeRegion srcRegion;
      if (srcKind == 0)      srcRegion = ctx.windows().shapeBounding(srcWid);
      else if (srcKind == 2) srcRegion = ctx.windows().shapeInput(srcWid);

      // Get dest shape and combine
      ShapeRegion destRegion;
      if (destKind == 0)      destRegion = ctx.windows().shapeBounding(destWid);
      else if (destKind == 1) destRegion = ctx.windows().shapeBounding(destWid);
      else if (destKind == 2) destRegion = ctx.windows().shapeInput(destWid);

      destRegion.combine(srcRegion, op, x_off, y_off, ww, wh);

      if (destKind == 0)      ctx.windows().setShapeBounding(destWid, std::move(destRegion));
      else if (destKind == 1) ctx.windows().setShapeClip(destWid, std::move(destRegion));
      else if (destKind == 2) ctx.windows().setShapeInput(destWid, std::move(destRegion));

      uint32_t host = ctx.windows().topLevelAncestorOf(destWid);
      if (host == 0) host = destWid;
      x11_ui_push_shape_changed(host);
      return;
    }
    case 4: {
      // ShapeOffset
      // Body: kind(1B), unused(1B), unused(2B), window(4B), x_off(2B), y_off(2B)
      if (br.remaining() < 12) { br.skip(br.remaining()); return; }
      const uint8_t  kind = br.readU8();
      br.skip(3); // unused
      const uint32_t wid  = br.readU32();
      const int16_t  dx   = (int16_t)br.readU16();
      const int16_t  dy   = (int16_t)br.readU16();
      br.skip(br.remaining());

      ShapeRegion region;
      if (kind == 0)      region = ctx.windows().shapeBounding(wid);
      else if (kind == 1) region = ctx.windows().shapeBounding(wid);
      else if (kind == 2) region = ctx.windows().shapeInput(wid);

      region.offset(dx, dy);

      if (kind == 0)      ctx.windows().setShapeBounding(wid, std::move(region));
      else if (kind == 1) ctx.windows().setShapeClip(wid, std::move(region));
      else if (kind == 2) ctx.windows().setShapeInput(wid, std::move(region));

      uint32_t host = ctx.windows().topLevelAncestorOf(wid);
      if (host == 0) host = wid;
      x11_ui_push_shape_changed(host);
      return;
    }
    case 5: {
      // ShapeQueryExtents — return actual shape data
      if (br.remaining() < 4) { br.skip(br.remaining()); return; }
      const uint32_t wid = br.readU32();
      br.skip(br.remaining());

      WindowView vw{};
      uint16_t ww = 0, wh = 0;
      if (ctx.windows().snapshot(wid, vw)) {
        ww = vw.w; wh = vw.h;
      }

      ShapeRegion bounding = ctx.windows().shapeBounding(wid);
      ShapeRegion::Rect bext = bounding.shaped ? bounding.extents() : ShapeRegion::Rect{0, 0, ww, wh};

      const bool bShaped = vw.bounding_shaped;
      const bool cShaped = vw.clip_shaped;

      (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0); // length
        rep[8]  = bShaped ? 1 : 0;
        rep[9]  = cShaped ? 1 : 0;
        wire::wr16_le(rep.data() + 12, (uint16_t)bext.x);
        wire::wr16_le(rep.data() + 14, (uint16_t)bext.y);
        wire::wr16_le(rep.data() + 16, bext.w);
        wire::wr16_le(rep.data() + 18, bext.h);
        // Clip extents: use bounding if no separate clip
        wire::wr16_le(rep.data() + 20, (uint16_t)bext.x);
        wire::wr16_le(rep.data() + 22, (uint16_t)bext.y);
        wire::wr16_le(rep.data() + 24, bext.w);
        wire::wr16_le(rep.data() + 26, bext.h);
      });
      return;
    }
    case 6: // ShapeSelectInput — consume silently (we don't track per-window shape event selection)
      br.skip(br.remaining());
      return;
    case 7: {
      // ShapeInputSelected — reply with enabled=false (we don't track selection)
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);
        rep[1] = 0; // enabled = false
      });
      return;
    }
    case 8: {
      // ShapeGetRectangles — return actual shape rectangles
      if (br.remaining() < 8) { br.skip(br.remaining()); return; }
      const uint32_t wid = br.readU32();
      const uint8_t kind = br.readU8(); // Bounding=0, Clip=1, Input=2
      br.skip(br.remaining());

      WindowView vw{};
      uint16_t ww = 0, wh = 0;
      if (ctx.windows().snapshot(wid, vw)) { ww = vw.w; wh = vw.h; }

      ShapeRegion region;
      if (kind == 0)      region = ctx.windows().shapeBounding(wid);
      else if (kind == 2) region = ctx.windows().shapeInput(wid);

      // If unshaped, return single full-window rect
      if (!region.shaped) {
        uint8_t rectPayload[8] = {};
        wire::wr16_le(rectPayload + 0, 0);
        wire::wr16_le(rectPayload + 2, 0);
        wire::wr16_le(rectPayload + 4, ww);
        wire::wr16_le(rectPayload + 6, wh);
        (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
          rep[1] = 0; // ordering = UnSorted
          wire::wr32_le(rep.data() + 4, 2); // length = 8/4 = 2 words
          wire::wr32_le(rep.data() + 8, 1); // nrects = 1
        });
        ctx.reply().sendBytes(rectPayload, sizeof(rectPayload));
      } else {
        const uint32_t nrects = (uint32_t)region.rects.size();
        const uint32_t payloadBytes = nrects * 8;
        const uint32_t payloadWords = (payloadBytes + 3) / 4;

        (void)ctx.reply().sendReply32(seq, [nrects, payloadWords](std::array<uint8_t, 32>& rep) {
          rep[1] = 0; // ordering = UnSorted
          wire::wr32_le(rep.data() + 4, payloadWords);
          wire::wr32_le(rep.data() + 8, nrects);
        });
        // Send each rectangle
        for (auto& r : region.rects) {
          uint8_t buf[8] = {};
          wire::wr16_le(buf + 0, (uint16_t)r.x);
          wire::wr16_le(buf + 2, (uint16_t)r.y);
          wire::wr16_le(buf + 4, r.w);
          wire::wr16_le(buf + 6, r.h);
          ctx.reply().sendBytes(buf, 8);
        }
        // Pad to 4-byte boundary if needed
        if (payloadBytes % 4 != 0) {
          uint8_t pad[4] = {};
          ctx.reply().sendBytes(pad, 4 - (payloadBytes % 4));
        }
      }
      return;
    }
    default:
      { char buf[128]; snprintf(buf, sizeof(buf), "[SHAPE] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
      br.skip(br.remaining());
      // Send error to prevent XCB sequence desync if sub-opcode was reply-bearing.
      ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
      return;
    }
  }

  // -------------------------------------------------------------------
  // RANDR — dynamic multi-monitor (version 1.3)
  //
  // Resource XIDs assigned per-monitor by ScreenLayout:
  //   Output  = 0x00100000 + 2*i
  //   CRTC    = 0x00100001 + 2*i
  //   Mode    = 0x00100010 + i
  // -------------------------------------------------------------------

  if (major == ext::kRANDR) {
    switch (minor) {

    case 0: {
      // RRQueryVersion — reply: 1.3
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);
        wire::wr32_le(rep.data() + 8, 1);  // major
        wire::wr32_le(rep.data() + 12, 3); // minor
      });
      return;
    }

    case 4: // RRSelectInput — void (event mask selection)
      br.skip(br.remaining());
      return;

    case 6: {
      // RRGetScreenSizeRange — reply min/max sizes
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);
        wire::wr16_le(rep.data() + 8,  1);    // min_width
        wire::wr16_le(rep.data() + 10, 1);    // min_height
        wire::wr16_le(rep.data() + 12, 8192); // max_width
        wire::wr16_le(rep.data() + 14, 8192); // max_height
      });
      return;
    }

    case 8:  // RRGetScreenResources      (1.2)
    case 25: // RRGetScreenResourcesCurrent (1.3)
    {
      br.skip(br.remaining());
      const auto layout = x11::getScreenLayout();
      const size_t N = layout.monitors.size();

      // Build mode names and ModeInfo blocks
      struct ModeEntry {
        uint8_t info[32];
        char    name[32];
        uint16_t nameLen;
      };
      std::vector<ModeEntry> modes(N);
      uint16_t totalNameLen = 0;

      for (size_t i = 0; i < N; i++) {
        const auto& m = layout.monitors[i];
        auto& me = modes[i];
        me.nameLen = static_cast<uint16_t>(
          std::snprintf(me.name, sizeof(me.name), "%ux%u", (unsigned)m.w, (unsigned)m.h));

        std::memset(me.info, 0, 32);
        wire::wr32_le(me.info + 0,  m.mode_xid);
        wire::wr16_le(me.info + 4,  m.w);          // width
        wire::wr16_le(me.info + 6,  m.h);          // height
        // Approximate timing: dotClock = w * h * 60
        uint32_t dotClock = static_cast<uint32_t>(m.w) * m.h * 60;
        wire::wr32_le(me.info + 8,  dotClock);
        wire::wr16_le(me.info + 12, m.w + 88);     // hSyncStart (approx)
        wire::wr16_le(me.info + 14, m.w + 132);    // hSyncEnd
        wire::wr16_le(me.info + 16, m.w + 280);    // hTotal
        wire::wr16_le(me.info + 18, 0);            // hSkew
        wire::wr16_le(me.info + 20, m.h + 4);      // vSyncStart
        wire::wr16_le(me.info + 22, m.h + 9);      // vSyncEnd
        wire::wr16_le(me.info + 24, m.h + 45);     // vTotal
        wire::wr16_le(me.info + 26, me.nameLen);   // nameLen
        wire::wr32_le(me.info + 28, 0);            // modeFlags

        totalNameLen += me.nameLen;
      }

      // Payload: crtc_ids[N] + output_ids[N] + ModeInfo[N] + names + pad
      size_t namesWithPad = (totalNameLen + 3u) & ~3u;
      size_t payloadBytes = 4*N + 4*N + 32*N + namesWithPad;
      std::vector<uint8_t> payload(payloadBytes, 0);
      size_t off = 0;

      // CRTC IDs
      for (size_t i = 0; i < N; i++) {
        wire::wr32_le(payload.data() + off, layout.monitors[i].crtc_xid);
        off += 4;
      }
      // Output IDs
      for (size_t i = 0; i < N; i++) {
        wire::wr32_le(payload.data() + off, layout.monitors[i].output_xid);
        off += 4;
      }
      // ModeInfo blocks
      for (size_t i = 0; i < N; i++) {
        std::memcpy(payload.data() + off, modes[i].info, 32);
        off += 32;
      }
      // Mode names (concatenated, padded to 4 bytes)
      for (size_t i = 0; i < N; i++) {
        std::memcpy(payload.data() + off, modes[i].name, modes[i].nameLen);
        off += modes[i].nameLen;
      }

      const uint32_t replyLength = static_cast<uint32_t>(payloadBytes / 4);
      const uint16_t numC = static_cast<uint16_t>(N);
      const uint16_t numO = static_cast<uint16_t>(N);
      const uint16_t numM = static_cast<uint16_t>(N);
      (void)ctx.reply().sendReply32(seq, [=](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, replyLength);
        wire::wr32_le(rep.data() + 8,  0);  // timestamp
        wire::wr32_le(rep.data() + 12, 0);  // configTimestamp
        wire::wr16_le(rep.data() + 16, numC);
        wire::wr16_le(rep.data() + 18, numO);
        wire::wr16_le(rep.data() + 20, numM);
        wire::wr16_le(rep.data() + 22, totalNameLen);
      });
      ctx.reply().sendBytes(payload.data(), payloadBytes);
      return;
    }

    case 9: {
      // RRGetOutputInfo — look up requested output
      // Reply is 36 bytes (not 32): has nClones and nameLength after nPreferred
      if (br.remaining() < 4) { br.skip(br.remaining()); return; }
      const uint32_t requested_output = br.readU32();
      br.skip(br.remaining());

      const auto layout = x11::getScreenLayout();
      const x11::MonitorInfo* found = nullptr;
      for (auto& m : layout.monitors) {
        if (m.output_xid == requested_output) { found = &m; break; }
      }
      if (!found) {
        ctx.transport().sendErrorCore(x11::error::BadValue, seq, requested_output, major);
        return;
      }

      const auto& mon = *found;
      const uint16_t nameLen = static_cast<uint16_t>(std::strlen(mon.name));

      // Payload after 36-byte header: crtcs(4) + modes(4) + clones(0) + name(nameLen) + pad
      size_t nameWithPad = (nameLen + 3u) & ~3u;
      size_t payloadBytes = 4 + 4 + nameWithPad;  // crtc_id + mode_id + name
      std::vector<uint8_t> payload(payloadBytes, 0);
      wire::wr32_le(payload.data() + 0, mon.crtc_xid);   // crtcs[0]
      wire::wr32_le(payload.data() + 4, mon.mode_xid);    // modes[0]
      // clones: 0 entries (nothing to write)
      std::memcpy(payload.data() + 8, mon.name, nameLen);  // name

      // 36-byte header (sz_xRRGetOutputInfoReply = 36)
      // length field = (36 - 32 + payloadBytes) / 4 = (4 + payloadBytes) / 4
      // which is 1 + payloadBytes/4
      const uint32_t replyLen = static_cast<uint32_t>((4 + payloadBytes) / 4);

      uint8_t rep[36] = {};
      rep[0] = 1;                                          // reply type
      rep[1] = 0;                                          // status = RRSetConfigSuccess
      wire::wr16_le(rep + 2, seq);                         // sequence
      wire::wr32_le(rep + 4, replyLen);                    // length
      wire::wr32_le(rep + 8, 0);                           // timestamp
      wire::wr32_le(rep + 12, mon.crtc_xid);              // crtc
      wire::wr32_le(rep + 16, mon.w_mm);                   // mm_width
      wire::wr32_le(rep + 20, mon.h_mm);                   // mm_height
      rep[24] = 0;                                          // connection = Connected
      rep[25] = 0;                                          // subpixel_order = Unknown
      wire::wr16_le(rep + 26, 1);                          // num_crtcs
      wire::wr16_le(rep + 28, 1);                          // num_modes
      wire::wr16_le(rep + 30, 1);                          // num_preferred
      wire::wr16_le(rep + 32, 0);                          // num_clones
      wire::wr16_le(rep + 34, nameLen);                    // nameLength

      ctx.reply().sendReplyRaw(rep, sizeof(rep));
      ctx.reply().sendBytes(payload.data(), payloadBytes);
      return;
    }

    case 10: {
      // RRListOutputProperties — empty list
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);
        wire::wr16_le(rep.data() + 8, 0); // num_atoms
      });
      return;
    }

    case 15: {
      // RRGetOutputProperty — return empty (no RANDR output properties stored)
      // Request: output(4) + property(4) + type(4) + long_offset(4) + long_length(4) + delete(1) + pending(1) + pad(2)
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 0;                            // format = 0 (property not found)
        wire::wr32_le(rep.data() + 4, 0);      // length = 0
        wire::wr32_le(rep.data() + 8, 0);      // type = None
        wire::wr32_le(rep.data() + 12, 0);     // bytes_after = 0
        wire::wr32_le(rep.data() + 16, 0);     // num_items = 0
      });
      return;
    }

    case 20: {
      // RRGetCrtcInfo — look up requested CRTC
      if (br.remaining() < 4) { br.skip(br.remaining()); return; }
      const uint32_t requested_crtc = br.readU32();
      br.skip(br.remaining());

      const auto layout = x11::getScreenLayout();
      const x11::MonitorInfo* found = nullptr;
      for (auto& m : layout.monitors) {
        if (m.crtc_xid == requested_crtc) { found = &m; break; }
      }
      if (!found) {
        ctx.transport().sendErrorCore(x11::error::BadValue, seq, requested_crtc, major);
        return;
      }

      const auto& mon = *found;

      // Payload: outputs(4) + possible_outputs(4) = 8 bytes
      uint8_t payload[8] = {};
      wire::wr32_le(payload + 0, mon.output_xid); // outputs[0]
      wire::wr32_le(payload + 4, mon.output_xid); // possible[0]

      const uint16_t mx = static_cast<uint16_t>(mon.x);
      const uint16_t my = static_cast<uint16_t>(mon.y);
      const uint16_t mw = mon.w;
      const uint16_t mh = mon.h;
      const uint32_t modeXid = mon.mode_xid;
      (void)ctx.reply().sendReply32(seq, [=](std::array<uint8_t, 32>& rep) {
        rep[1] = 0; // status
        wire::wr32_le(rep.data() + 4, 2);        // length = 8/4
        wire::wr32_le(rep.data() + 8, 0);        // timestamp
        wire::wr16_le(rep.data() + 12, mx);      // x
        wire::wr16_le(rep.data() + 14, my);      // y
        wire::wr16_le(rep.data() + 16, mw);      // width
        wire::wr16_le(rep.data() + 18, mh);      // height
        wire::wr32_le(rep.data() + 20, modeXid); // mode
        wire::wr16_le(rep.data() + 24, 1);       // rotation = Rotate_0
        wire::wr16_le(rep.data() + 26, 1);       // rotations = Rotate_0
        wire::wr16_le(rep.data() + 28, 1);       // num_outputs
        wire::wr16_le(rep.data() + 30, 1);       // num_possible
      });
      ctx.reply().sendBytes(payload, sizeof(payload));
      return;
    }

    case 21: {
      // RRSetCrtcConfig — reply success
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 0; // status = RRSetConfigSuccess
        wire::wr32_le(rep.data() + 4, 0);
        wire::wr32_le(rep.data() + 8, 0); // timestamp
      });
      return;
    }

    case 22: {
      // RRGetCrtcGammaSize — reply size=0
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);
        wire::wr16_le(rep.data() + 8, 0); // size
      });
      return;
    }

    case 31: {
      // RRGetOutputPrimary — reply with primary output XID
      br.skip(br.remaining());
      const auto layout = x11::getScreenLayout();
      uint32_t primaryOutput = 0x00100000; // fallback
      for (auto& m : layout.monitors) {
        if (m.is_primary) { primaryOutput = m.output_xid; break; }
      }
      (void)ctx.reply().sendReply32(seq, [primaryOutput](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);
        wire::wr32_le(rep.data() + 8, primaryOutput);
      });
      return;
    }

    case 27: {
      // RRGetCrtcTransform — 96-byte reply: identity transforms, no filters
      // xRRGetCrtcTransformReply = 96 bytes:
      //   0-7:   reply header (type, status, seq, length)
      //   8-43:  pendingTransform (xRenderTransform = 9 x Fixed = 36 bytes)
      //   44:    hasTransforms (BOOL)
      //   45-47: pad
      //   48-83: currentTransform (xRenderTransform = 9 x Fixed)
      //   84-87: pad
      //   88-89: pendingNbytesFilter
      //   90-91: pendingNparamsFilter
      //   92-93: currentNbytesFilter
      //   94-95: currentNparamsFilter
      br.skip(br.remaining());
      uint8_t rep[96] = {};
      rep[0] = 1;                          // reply type
      rep[1] = 1;                          // status = hasTransforms
      wire::wr16_le(rep + 2, seq);         // sequence
      wire::wr32_le(rep + 4, 16);          // length = (96-32)/4 = 16

      // Identity transform: diag = 1.0 in 16.16 fixed-point = 0x00010000
      const uint32_t one = 0x00010000u;
      // Pending transform (bytes 8-43): 3x3 identity
      wire::wr32_le(rep + 8,  one);        // m11
      wire::wr32_le(rep + 24, one);        // m22
      wire::wr32_le(rep + 40, one);        // m33
      rep[44] = 1;                          // hasTransforms = true
      // Current transform (bytes 48-83): 3x3 identity
      wire::wr32_le(rep + 48, one);        // m11
      wire::wr32_le(rep + 64, one);        // m22
      wire::wr32_le(rep + 80, one);        // m33
      // Filter name lengths = 0 (bytes 88-95 already zero)
      ctx.reply().sendReplyRaw(rep, sizeof(rep));
      return;
    }

    case 28: {
      // RRGetPanning — 36-byte reply: no panning (all zeros)
      br.skip(br.remaining());
      uint8_t rep[36] = {};
      rep[0] = 1;                        // reply type
      rep[1] = 0;                        // status = success
      wire::wr16_le(rep + 2, seq);       // sequence
      wire::wr32_le(rep + 4, 1);         // length = (36-32)/4 = 1
      // bytes 8-35: all zero (timestamp=0, no panning rect, no tracking, no borders)
      ctx.reply().sendReplyRaw(rep, sizeof(rep));
      return;
    }

    case 29: {
      // RRSetPanning — reply with success status
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 0; // status = RRSetConfigSuccess
        wire::wr32_le(rep.data() + 4, 0);
        wire::wr32_le(rep.data() + 8, 0); // timestamp
      });
      return;
    }

    case 32: {
      // RRGetProviders — reply with empty provider list
      // RANDR 1.4 feature; xrandr sends this but we don't need provider support
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);
        wire::wr32_le(rep.data() + 8, 0); // timestamp
        wire::wr16_le(rep.data() + 12, 0); // num_providers
      });
      return;
    }

    default:
      { char buf[128]; snprintf(buf, sizeof(buf), "[RANDR] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
      br.skip(br.remaining());
      {
        // Send error with actual minor opcode (not 0) so client reports correctly
        auto e = x11::wireerr::buildError32(x11::error::BadRequest, seq, 0, minor, major);
        ctx.transport().sendAll(e.data(), e.size());
      }
      return;
    }
  }

  // -------------------------------------------------------------------
  // Xinerama — dynamic multi-monitor
  // minor 0 = QueryVersion, minor 4 = IsActive, minor 5 = QueryScreens
  // -------------------------------------------------------------------
  if (major == ext::kXinerama) {
    if (minor == 0) {
      // XineramaQueryVersion: BYTE client_major, BYTE client_minor
      br.skip(br.remaining());
      // Reply: major=1, minor=1
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);    // length
        wire::wr16_le(rep.data() + 8, 1);    // major version
        wire::wr16_le(rep.data() + 10, 1);   // minor version
      });
      return;
    }
    if (minor == 4) {
      // XineramaIsActive — no request body
      br.skip(br.remaining());
      // Reply: state=1 (active)
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0); // length
        wire::wr32_le(rep.data() + 8, 1); // state = active
      });
      return;
    }
    if (minor == 5) {
      // XineramaQueryScreens — dynamic from ScreenLayout
      br.skip(br.remaining());

      const auto layout = x11::getScreenLayout();
      const size_t N = layout.monitors.size();

      // Each XineramaScreenInfo = 8 bytes: INT16 x, INT16 y, CARD16 w, CARD16 h
      std::vector<uint8_t> payload(N * 8, 0);
      for (size_t i = 0; i < N; i++) {
        const auto& m = layout.monitors[i];
        wire::wr16_le(payload.data() + i*8 + 0, static_cast<uint16_t>(m.x));
        wire::wr16_le(payload.data() + i*8 + 2, static_cast<uint16_t>(m.y));
        wire::wr16_le(payload.data() + i*8 + 4, m.w);
        wire::wr16_le(payload.data() + i*8 + 6, m.h);
      }

      const uint32_t replyLength = static_cast<uint32_t>(N * 2); // N*8/4
      const uint32_t numScreens = static_cast<uint32_t>(N);
      (void)ctx.reply().sendReply32(seq, [=](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, replyLength);
        wire::wr32_le(rep.data() + 8, numScreens);
      });
      ctx.reply().sendBytes(payload.data(), N * 8);
      return;
    }
    { char buf[128]; snprintf(buf, sizeof(buf), "[Xinerama] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
    br.skip(br.remaining());
    // Send error to prevent XCB sequence desync if sub-opcode was reply-bearing.
    ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
    return;
  }

  // -------------------------------------------------------------------
  // Generic Events (GE) — minor 0 = GEQueryVersion
  // -------------------------------------------------------------------
  if (major == ext::kGE) {
    if (minor == 0) {
      // GEQueryVersion: CARD16 client_major, CARD16 client_minor
      br.skip(br.remaining());
      // Reply: major=1, minor=0
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);    // length
        wire::wr16_le(rep.data() + 8, 1);    // major version
        wire::wr16_le(rep.data() + 10, 0);   // minor version
      });
      return;
    }
    { char buf[128]; snprintf(buf, sizeof(buf), "[GE] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
    br.skip(br.remaining());
    // Send error to prevent XCB sequence desync if sub-opcode was reply-bearing.
    ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
    return;
  }

  // -------------------------------------------------------------------
  // XC-MISC — XID range recycling (prevents exhaustion in long sessions)
  // -------------------------------------------------------------------
  if (major == ext::kXCMisc) {
    switch (minor) {
    case 0: {
      // XC-MiscGetVersion — reply: server major=1, minor=1
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);    // length
        wire::wr16_le(rep.data() + 8, 1);    // server major version
        wire::wr16_le(rep.data() + 10, 1);   // server minor version
      });
      return;
    }
    case 1: {
      // XC-MiscGetXIDRange — reply: (start_id, count)
      br.skip(br.remaining());
      auto* client = ctx.client();
      uint32_t start_id = 0, count = 0;
      if (client) {
        auto [s, c] = client->allocXIDRange(65536);
        start_id = s;
        count = c;
      }
      (void)ctx.reply().sendReply32(seq, [start_id, count](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);           // length
        wire::wr32_le(rep.data() + 8, start_id);    // start_id
        wire::wr32_le(rep.data() + 12, count);      // count
      });
      return;
    }
    case 2: {
      // XC-MiscGetXIDList — request: CARD32 count; reply: CARD32 ids_count, [CARD32 id…]
      const uint32_t requested = br.remaining() >= 4 ? br.readU32() : 0;
      br.skip(br.remaining());
      auto* client = ctx.client();
      // Cap at 4096 to avoid huge allocations
      const uint32_t cap = std::min(requested, uint32_t(4096));
      std::vector<uint32_t> ids(cap);
      uint32_t actual = 0;
      if (client)
        actual = client->allocXIDList(ids.data(), cap);
      const uint32_t payload_words = actual;  // each ID is 1 CARD32
      (void)ctx.reply().sendReply32(seq, [payload_words, actual](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, payload_words);  // length (extra 4-byte words)
        wire::wr32_le(rep.data() + 8, actual);          // ids_count
      });
      if (actual > 0) {
        // Convert to little-endian wire format
        std::vector<uint8_t> payload(actual * 4);
        for (uint32_t i = 0; i < actual; ++i)
          wire::wr32_le(payload.data() + i * 4, ids[i]);
        ctx.reply().sendBytes(payload.data(), payload.size());
      }
      return;
    }
    default:
      break;
    }
    { char buf[128]; snprintf(buf, sizeof(buf), "[XC-MISC] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
    br.skip(br.remaining());
    ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
    return;
  }

  // -------------------------------------------------------------------
  // XINPUT2 (XInput2) — major opcode 141
  // -------------------------------------------------------------------
  if (major == ext::kXInput2) {
    { char buf[128]; snprintf(buf, sizeof(buf), "[XInput2] fd=%d minor=%u seq=%u\n",
        ctx.transport().clientFd(), (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
    switch (minor) {

    // ---- minor 1: GetExtensionVersion (XI1 legacy — reply-bearing) ----
    case 1: {
      // libXi sends this before XIQueryVersion.
      // Request: CARD16 name_len, pad16, then name bytes.
      br.skip(br.remaining());
      // Reply 32 bytes: major=2, minor=0, present=1
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);     // length
        wire::wr16_le(rep.data() + 8, 2);     // server_major
        wire::wr16_le(rep.data() + 10, 0);    // server_minor
        rep[12] = 1;                            // present = True
      });
      return;
    }

    // ---- minor 2: ListInputDevices (XI1 — reply-bearing) ----
    case 2: {
      // Return the 4 virtual core devices matching XIQueryDevice (minor 48).
      // Chromium calls both ListInputDevices AND XIQueryDevice in parallel.
      // Wire format: 32-byte header + ndevices * xDeviceInfo(8B) +
      //   InputClassInfo entries + name strings (STR: 1B len + chars) + pad4.
      br.skip(br.remaining());

      // Device definitions
      struct XI1Dev {
        uint8_t id;
        uint8_t use;       // 0=IsXPointer, 1=IsXKeyboard, 2=IsXExtensionDevice,
                           // 3=IsXExtensionPointer, 4=IsXExtensionKeyboard
        uint8_t attached;
        const char* name;
        uint8_t num_classes; // number of InputClassInfo entries
      };
      const XI1Dev devs[] = {
        { 2, 0, 0, "Virtual core pointer",         2 },  // ButtonInfo + ValuatorInfo
        { 3, 1, 0, "Virtual core keyboard",         1 },  // KeyInfo
        { 4, 3, 2, "Virtual core XTEST pointer",   2 },  // ButtonInfo + ValuatorInfo
        { 5, 4, 3, "Virtual core XTEST keyboard",  1 },  // KeyInfo
      };
      constexpr uint8_t ndevices = 4;

      // Build payload: xDeviceInfo array, then InputClassInfo array, then name strings
      std::vector<uint8_t> payload;
      auto push8  = [&](uint8_t v) { payload.push_back(v); };
      auto push16 = [&](uint16_t v) { uint8_t b[2]; wire::wr16_le(b, v); payload.insert(payload.end(), b, b+2); };
      auto push32 = [&](uint32_t v) { uint8_t b[4]; wire::wr32_le(b, v); payload.insert(payload.end(), b, b+4); };

      // Section 1: ndevices * xDeviceInfo (8 bytes each)
      for (const auto& d : devs) {
        push32(0);              // type atom (0 = None)
        push8(d.id);           // device id
        push8(d.num_classes);  // num_classes
        push8(d.use);          // use
        push8(d.attached);     // attached
      }

      // Section 2: InputClassInfo entries (in device order)
      for (const auto& d : devs) {
        if (d.use == 0 || d.use == 3) { // pointer devices
          // xButtonInfo: class=1, length=4, num_buttons=5
          push8(1); push8(4); push16(5);
          // xValuatorInfo: class=2, length=8, num_axes=0, mode=0(Relative), motion_buffer_size=0
          push8(2); push8(8); push8(0); push8(0); push32(0);
        } else { // keyboard devices
          // xKeyInfo: class=0, length=8, min_keycode=8, max_keycode=255, num_keys=248, pad=0
          push8(0); push8(8); push8(8); push8(255); push16(248); push16(0);
        }
      }

      // Section 3: name strings (STR format: 1-byte length + chars, no null terminator)
      for (const auto& d : devs) {
        uint8_t len = (uint8_t)std::strlen(d.name);
        push8(len);
        payload.insert(payload.end(), d.name, d.name + len);
      }

      // Pad to 4-byte alignment
      while (payload.size() % 4u) payload.push_back(0);

      // Build reply header + payload
      const uint32_t payload_words = (uint32_t)(payload.size() / 4u);
      std::vector<uint8_t> reply(32 + payload.size(), 0);
      reply[0] = 1;  // Reply
      reply[1] = 2;  // XI reply type = ListInputDevices
      wire::wr16_le(reply.data() + 2, seq);
      wire::wr32_le(reply.data() + 4, payload_words);
      reply[8] = ndevices;
      std::memcpy(reply.data() + 32, payload.data(), payload.size());
      (void)ctx.reply().sendReplyRaw(reply.data(), reply.size());
      return;
    }

    // ---- minor 7: GrabDevice (XI1 reply-bearing) ----
    case 7: {
      // Request: CARD32 grab_window, CARD32 time, CARD16 num_classes,
      //          CARD8 this_device_mode, CARD8 other_device_mode,
      //          BOOL owner_events, CARD8 deviceid, CARD16 pad,
      //          then class list.
      br.skip(br.remaining());
      // Reply: status = Success (0).
      // xGrabDeviceReply: repType(1), RepType(byte1), seq(2-3),
      //   length(4-7)=0, status(8), pad...
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);  // length = 0
        rep[8] = 0;                          // status = Success
      });
      { char buf[128]; snprintf(buf, sizeof(buf), "[XInput2] XI1 GrabDevice minor=7 seq=%u — replied Success\n", (unsigned)seq); x11_ui_push_log(1, buf); }
      return;
    }

    // ---- minor 10: GetDeviceFocus (XI1 reply-bearing) ----
    case 10: {
      // Request: CARD8 deviceid, pad*3
      br.skip(br.remaining());
      // Reply: focus window, time, revert-to.
      // xGetDeviceFocusReply: repType(1), RepType(byte1), seq(2-3),
      //   length(4-7)=0, focus(8-11), time(12-15), revertTo(16), pad...
      const uint32_t focusWin = ctx.input().focus_xid ? ctx.input().focus_xid : 1; // PointerRoot=1
      (void)ctx.reply().sendReply32(seq, [focusWin](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);         // length = 0
        wire::wr32_le(rep.data() + 8, focusWin);  // focus window
        wire::wr32_le(rep.data() + 12, 0);        // time = CurrentTime
        rep[16] = 1;                                // revertTo = PointerRoot
      });
      { char buf[128]; snprintf(buf, sizeof(buf), "[XInput2] XI1 GetDeviceFocus minor=10 seq=%u focus=0x%X\n", (unsigned)seq, focusWin); x11_ui_push_log(1, buf); }
      return;
    }

    // ---- minor 24: QueryDeviceState (XI1 reply-bearing) ----
    case 24: {
      // Request: CARD8 deviceid, pad*3
      br.skip(br.remaining());
      // Reply: num_classes=0, no trailing class data.
      // xQueryDeviceStateReply: repType(1), RepType(byte1), seq(2-3),
      //   length(4-7)=0, num_classes(8), pad...
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);  // length = 0 (no trailing classes)
        rep[8] = 0;                          // num_classes = 0
      });
      { char buf[128]; snprintf(buf, sizeof(buf), "[XInput2] XI1 QueryDeviceState minor=24 seq=%u\n", (unsigned)seq); x11_ui_push_log(1, buf); }
      return;
    }

    // ---- remaining XI1 legacy reply-bearing stubs (send error to keep XCB in sync) ----
    case 3:  // GetDeviceDontPropagateList (reply)
    case 4:  // GetDeviceMotionEvents (reply)
    case 5:  // ChangeKeyboardDevice (reply)
    case 6:  // ChangePointerDevice (reply)
    case 14: // GetFeedbackControl (reply)
    case 18: // GetDeviceKeyMapping (reply)
    case 20: // GetDeviceModifierMapping (reply)
    case 22: // GetDeviceButtonMapping (reply)
    case 30: // GetSelectedExtensionEvents (reply)
    case 31: // GetDeviceInfo (reply, XI1.5)
      { char buf[128]; snprintf(buf, sizeof(buf), "[XInput2] XI1 reply-bearing minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
      br.skip(br.remaining());
      ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
      return;

    // ---- XI1 legacy void stubs (no reply expected) ----
    case 8:  // UngrabDevice
    case 9:  // FocusIn/FocusOut (event, not request)
    case 11: // SetDeviceFocus
    case 12: // ChangeFeedbackControl
    case 13: // GetDeviceModifierMapping — already covered above
    case 15: // ChangeDeviceDontPropagateList
    case 16: // GetDeviceMotionEvents — already covered above
    case 17: // ChangeDeviceKeyMapping
    case 19: // ChangeDeviceKeyMapping
    case 21: // SetDeviceModifierMapping
    case 23: // SetDeviceButtonMapping
    case 25: // SendExtensionEvent
    case 26: case 27: case 28: case 29:
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
      br.skip(br.remaining());
      return;

    // ---- minor 40: XIQueryPointer (reply-bearing) ----
    case 40: {
      br.skip(br.remaining());
      // Reply: root=kRootXid, child=0, root_x/y=0, win_x/y=0, buttons_len=1, mods/group=0
      std::array<uint8_t, 56> rep{};
      rep[0] = 1;  // reply
      wire::wr16_le(rep.data() + 2, seq);
      wire::wr32_le(rep.data() + 4, 6);   // length = 6 extra words (24 bytes)
      wire::wr32_le(rep.data() + 8, 1);   // root window
      wire::wr32_le(rep.data() + 12, 0);  // child
      // root_x/y, win_x/y all 0 (FP16.16 format — 32-bit each)
      wire::wr16_le(rep.data() + 36, 1);  // buttons_len = 1
      // mods (base/latched/locked/effective) = 0, group = 0
      // buttons mask (4 bytes at offset 56-4=52) — all zeros
      ctx.transport().sendAll(rep.data(), rep.size());
      return;
    }

    // ---- minor 41-45: XI2 void stubs ----
    case 41: // XIWarpPointer
    case 42: // XIChangeCursor
    case 43: // XIChangeHierarchy
    case 44: // XISetClientPointer
      br.skip(br.remaining());
      return;
    case 45: { // XIGetClientPointer (reply-bearing)
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0); // length
        rep[1] = 1;                         // set = True
        wire::wr16_le(rep.data() + 8, 2);  // deviceid = virtual core pointer
      });
      return;
    }

    // ---- minor 46: XISelectEvents (void — no reply) ----
    case 46: {
      // XISelectEvents: CARD32 window, CARD16 num_masks, pad16
      //   then per mask: CARD16 deviceid, CARD16 mask_len, mask_len*4 bytes
      if (br.remaining() < 8) { br.skip(br.remaining()); return; }
      uint32_t window = br.readU32();
      uint16_t num_masks = br.readU16();
      br.skip(2); // pad
      uint32_t combined_mask = 0;
      for (uint16_t i = 0; i < num_masks && br.remaining() >= 4; i++) {
        uint16_t deviceid = br.readU16();
        uint16_t mask_len = br.readU16(); // in 4-byte units
        (void)deviceid;
        uint32_t mask = 0;
        for (uint16_t j = 0; j < mask_len && br.remaining() >= 4; j++) {
          uint32_t word = br.readU32();
          if (j == 0) mask = word;  // only first word has event types 0-31
        }
        combined_mask |= mask;
      }
      br.skip(br.remaining()); // consume any trailing padding
      // Root window (XID 1) isn't in WindowTable — store in InputState.
      if (window == 1) {
        ctx.input().xi2_root_mask = combined_mask;
      } else {
        ctx.windows().setXI2Mask(window, combined_mask);
      }
      return;
    }

    // ---- minor 47: XIQueryVersion (reply-bearing) ----
    case 47: {
      // Request: CARD16 client_major_version, CARD16 client_minor_version
      br.skip(br.remaining());
      // Reply: server_major=2, server_minor=2
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);     // length (no extra data)
        wire::wr16_le(rep.data() + 8, 2);     // server_major_version
        wire::wr16_le(rep.data() + 10, 2);    // server_minor_version (2.2 for ScrollClass)
      });
      return;
    }

    // ---- minor 48: XIQueryDevice (reply-bearing with payload) ----
    case 48: {
      // Request: CARD16 deviceid (0=XIAllDevices, 1=XIAllMasterDevices, or specific)
      const uint16_t requested_device = br.remaining() >= 2 ? br.readU16() : 0;
      br.skip(br.remaining());
      const auto layout = x11::getScreenLayout();
      { char buf[128]; snprintf(buf, sizeof(buf),
          "[XIQueryDevice] seq=%u requested_device=%u\n",
          (unsigned)seq, (unsigned)requested_device);
        x11_ui_push_log(1, buf); fprintf(stderr, "%s", buf); }

      // Build XIDeviceInfo entries for the 4 virtual core devices.
      // XIDeviceInfo wire format (per device):
      //   CARD16 deviceid, CARD16 type(use), CARD16 attachment,
      //   CARD16 num_classes, CARD16 name_len, CARD8 enabled, CARD8 pad
      //   + name bytes padded to 4
      //   + class entries

      struct DeviceDesc {
        uint16_t id;
        uint16_t use;        // 1=MasterPointer, 2=MasterKeyboard, 3=SlavePointer, 4=SlaveKeyboard
        uint16_t attachment;
        const char* name;
        bool has_buttons;    // pointer devices get ButtonClass + ValuatorClass (X,Y)
        bool has_keys;       // keyboard devices get KeyClass
      };

      static const DeviceDesc allDevices[] = {
        { 2, 1, 3, "Virtual core pointer",         true,  false },
        { 3, 2, 2, "Virtual core keyboard",        false, true  },  // attachment=2 (paired master pointer)
        { 4, 3, 2, "Virtual core XTEST pointer",   true,  false },
        { 5, 4, 3, "Virtual core XTEST keyboard",  false, true  },
      };

      // Filter devices based on request
      std::vector<const DeviceDesc*> devices;
      for (const auto& d : allDevices) {
        if (requested_device == 0 /* XIAllDevices */ ||
            requested_device == 1 /* XIAllMasterDevices (masters only) */ ||
            d.id == requested_device) {
          if (requested_device == 1 && d.use > 2) continue; // skip slaves for XIAllMasterDevices
          devices.push_back(&d);
        }
      }

      // Build payload
      std::vector<uint8_t> payload;
      auto appendU16 = [&](uint16_t v) {
        uint8_t b[2]; wire::wr16_le(b, v); payload.insert(payload.end(), b, b + 2);
      };
      auto appendU32 = [&](uint32_t v) {
        uint8_t b[4]; wire::wr32_le(b, v); payload.insert(payload.end(), b, b + 4);
      };

      for (const auto* dev : devices) {
        const uint16_t name_len = (uint16_t)std::strlen(dev->name);
        const uint16_t name_pad = (4 - (name_len % 4)) % 4;
        uint16_t num_classes = 0;
        if (dev->has_buttons) num_classes += 5; // ButtonClass + 2 ValuatorClass + 2 ScrollClass
        if (dev->has_keys) num_classes++;

        // XIDeviceInfo header (12 bytes)
        appendU16(dev->id);
        appendU16(dev->use);
        appendU16(dev->attachment);
        appendU16(num_classes);
        appendU16(name_len);
        payload.push_back(1);  // enabled
        payload.push_back(0);  // pad

        // Name + padding
        payload.insert(payload.end(), dev->name, dev->name + name_len);
        for (uint16_t i = 0; i < name_pad; i++) payload.push_back(0);

        // ButtonClass for pointer devices
        if (dev->has_buttons) {
          // ButtonClass: type=0, length_words, sourceid, num_buttons,
          //   state_mask (ceil(num_buttons/32)*4 bytes), labels[num_buttons]
          const uint16_t num_buttons = 5;
          const uint16_t state_words = 1; // ceil(5/32) = 1 word = 4 bytes
          // Total bytes: 8 (header) + state_words*4 + num_buttons*4
          const uint16_t total_bytes = 8 + state_words * 4 + num_buttons * 4;
          const uint16_t length_words = total_bytes / 4;

          appendU16(1);              // type = ButtonClass (XI2: 1)
          appendU16(length_words);   // length in 4-byte words
          appendU16(dev->id);        // sourceid
          appendU16(num_buttons);    // num_buttons
          // Button state mask (all released = 0)
          appendU32(0);
          // Labels (atom per button — 0=None for all)
          for (uint16_t b = 0; b < num_buttons; b++) appendU32(0);

          // ValuatorClass for X axis (axis 0)
          // Format: type(2) + length(2) + sourceid(2) + number(2) +
          //         label(4) + min(4+4 FP3232) + max(4+4) + value(4+4) +
          //         resolution(4) + mode(1) + pad(3)
          // Total: 2+2+2+2+4+8+8+8+4+1+3 = 44 bytes = 11 words
          appendU16(2);              // type = ValuatorClass
          appendU16(11);             // length = 11 words
          appendU16(dev->id);        // sourceid
          appendU16(0);              // number = 0 (X axis)
          appendU32(0);              // label = None
          // min: FP3232 = 0.0
          appendU32(0); appendU32(0);
          // max: FP3232 = screen width
          appendU32((uint32_t)layout.virtual_w); appendU32(0);
          // value: FP3232 = 0.0
          appendU32(0); appendU32(0);
          appendU32(1);              // resolution
          payload.push_back(1);      // mode = Absolute (master pointer uses Absolute)
          payload.push_back(0);      // pad
          payload.push_back(0);      // pad
          payload.push_back(0);      // pad

          // ValuatorClass for Y axis (axis 1) — same structure
          appendU16(2);              // type = ValuatorClass
          appendU16(11);             // length = 11 words
          appendU16(dev->id);        // sourceid
          appendU16(1);              // number = 1 (Y axis)
          appendU32(0);              // label = None
          appendU32(0); appendU32(0);  // min
          appendU32((uint32_t)layout.virtual_h); appendU32(0); // max (screen height)
          appendU32(0); appendU32(0);  // value
          appendU32(1);              // resolution
          payload.push_back(1);      // mode = Absolute
          payload.push_back(0);
          payload.push_back(0);
          payload.push_back(0);

          // ScrollClass for vertical scroll (axis 0, mapped to Y valuator)
          // xXIScrollInfo: 24 bytes = 6 words
          appendU16(3);              // type = ScrollClass (XI2: 3)
          appendU16(6);              // length = 6 words
          appendU16(dev->id);        // sourceid
          appendU16(0);              // number = 0 (axis number, maps to valuator 0)
          appendU16(0);              // scroll_type = 0 (Vertical)
          appendU16(0);              // pad
          appendU32(2);              // flags = XIScrollFlagPreferred (2)
          // increment: FP3232 = 1.0 (one scroll unit per click)
          appendU32(1); appendU32(0);

          // ScrollClass for horizontal scroll (axis 1)
          appendU16(3);              // type = ScrollClass
          appendU16(6);              // length = 6 words
          appendU16(dev->id);        // sourceid
          appendU16(1);              // number = 1 (axis number, maps to valuator 1)
          appendU16(1);              // scroll_type = 1 (Horizontal)
          appendU16(0);              // pad
          appendU32(0);              // flags = 0
          appendU32(1); appendU32(0); // increment = 1.0
        }

        // KeyClass for keyboard devices
        if (dev->has_keys) {
          // KeyClass: type=1, length_words, sourceid, num_keycodes, keycodes[]
          // Report keycodes 8..255 (standard X11 range)
          const uint16_t num_keycodes = 248; // 8..255
          const uint16_t total_bytes = 8 + num_keycodes * 4;
          const uint16_t length_words = total_bytes / 4;

          appendU16(0);              // type = KeyClass (XI2: 0)
          appendU16(length_words);   // length in 4-byte words
          appendU16(dev->id);        // sourceid
          appendU16(num_keycodes);   // num_keycodes
          // Keycodes 8..255
          for (uint16_t k = 8; k <= 255; k++) appendU32(k);
        }
      }

      // Pad payload to 4-byte boundary (should already be aligned)
      while (payload.size() % 4u) payload.push_back(0);

      const uint32_t payload_words = (uint32_t)(payload.size() / 4u);
      const uint16_t num_devices = (uint16_t)devices.size();

      // Build combined reply: 32-byte header + payload in one buffer
      // to ensure a single sendAll() call (no interleaving opportunity).
      std::vector<uint8_t> reply(32 + payload.size(), 0);
      reply[0] = 1; // Reply
      reply[1] = 0;
      wire::wr16_le(reply.data() + 2, seq);
      wire::wr32_le(reply.data() + 4, payload_words);
      wire::wr16_le(reply.data() + 8, num_devices);
      std::memcpy(reply.data() + 32, payload.data(), payload.size());

      // Full hex dump to stderr for debugging
      {
        fprintf(stderr, "[XIQueryDevice] FULL REPLY (%zu bytes):\n", reply.size());
        for (size_t i = 0; i < reply.size(); i += 16) {
          fprintf(stderr, "  %04zX: ", i);
          for (size_t j = i; j < i + 16 && j < reply.size(); j++)
            fprintf(stderr, "%02X ", reply[j]);
          fprintf(stderr, "\n");
        }
      }

      (void)ctx.reply().sendReplyRaw(reply.data(), reply.size());
      return;
    }

    // ---- minor 49: XISetFocus (void) ----
    case 49:
      br.skip(br.remaining());
      return;

    // ---- minor 50: XIGetFocus (reply-bearing) ----
    case 50: {
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);  // length
        wire::wr32_le(rep.data() + 8, 1);  // focus = PointerRoot (1)
      });
      return;
    }

    // ---- minor 51: XIGrabDevice (reply-bearing) ----
    case 51: {
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);  // length
        rep[1] = 0;                          // status = Success
      });
      return;
    }

    // ---- minor 52-55: XI2 stubs ----
    case 52: // XIUngrabDevice (void)
    case 53: // XIAllowEvents (void)
    case 55: // XIPassiveUngrabDevice (void)
      br.skip(br.remaining());
      return;

    case 54: // XIPassiveGrabDevice (reply-bearing)
      { char buf[128]; snprintf(buf, sizeof(buf), "[XInput2] XIPassiveGrabDevice minor=54 seq=%u — sending BadRequest\n", (unsigned)seq); x11_ui_push_log(1, buf); }
      br.skip(br.remaining());
      ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
      return;

    // ---- minor 56: XIListProperties (reply-bearing) ----
    case 56: {
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);   // length
        wire::wr16_le(rep.data() + 8, 0);   // num_properties = 0
      });
      return;
    }

    // ---- minor 57-58: XI2 void stubs ----
    case 57: // XIChangeProperty
    case 58: // XIDeleteProperty
      br.skip(br.remaining());
      return;

    // ---- minor 59: XIGetProperty (reply-bearing) ----
    case 59: {
      br.skip(br.remaining());
      // Reply: type=0 (None), bytes_after=0, num_items=0
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);   // length
        rep[1] = 0;                           // result_format = 0
        wire::wr32_le(rep.data() + 8, 0);   // type = None
        wire::wr32_le(rep.data() + 12, 0);  // bytes_after
        wire::wr32_le(rep.data() + 16, 0);  // num_items
      });
      return;
    }

    // ---- minor 60: XIGetSelectedEvents (reply-bearing) ----
    case 60: {
      // XIGetSelectedEvents: CARD32 window
      uint32_t window = (br.remaining() >= 4) ? br.readU32() : 0;
      br.skip(br.remaining());
      // Look up stored XI2 mask for this window
      x11::WindowView wv;
      bool found = ctx.windows().snapshot(window, wv);
      uint32_t mask = found ? wv.xi2_mask : 0;
      if (mask == 0) {
        // No XI2 selection — return empty
        (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
          wire::wr32_le(rep.data() + 4, 0);
          wire::wr16_le(rep.data() + 8, 0);
        });
      } else {
        // Return one mask entry: 8 bytes (deviceid=0 XIAllDevices, mask_len=1, mask)
        (void)ctx.reply().sendReply32(seq, [mask](std::array<uint8_t, 32>& rep) {
          wire::wr32_le(rep.data() + 4, 2);   // length = 2 words (8 bytes extra)
          wire::wr16_le(rep.data() + 8, 1);   // num_masks = 1
        });
        // Send trailing data: deviceid(2) + mask_len(2) + mask(4) = 8 bytes
        uint8_t extra[8] = {};
        wire::wr16_le(extra + 0, 0);    // deviceid = XIAllDevices
        wire::wr16_le(extra + 2, 1);    // mask_len = 1 (one 4-byte word)
        wire::wr32_le(extra + 4, mask); // the actual mask
        ctx.transport().sendAll(extra, 8);
      }
      return;
    }

    default:
      break;
    }
    // Unhandled XI2 minor — send BadRequest so XCB sequence stays in sync.
    // Without an error reply, a reply-bearing minor we haven't implemented
    // would leave XCB hanging, eventually causing a sequence desync crash.
    { char buf[128]; snprintf(buf, sizeof(buf), "[XInput2] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
    br.skip(br.remaining());
    ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
    return;
  }

  // -------------------------------------------------------------------
  // XTEST — major opcode 142
  // -------------------------------------------------------------------
  if (major == ext::kXTEST) {
    switch (minor) {

    // ---- minor 0: XTestGetVersion (reply-bearing) ----
    case 0: {
      br.skip(br.remaining());
      std::array<uint8_t, 32> rep{};
      rep.fill(0);
      rep[0] = 1;                            // Reply
      rep[1] = 2;                            // server_major_version
      wire::wr16_le(rep.data() + 2, seq);   // sequence
      wire::wr32_le(rep.data() + 4, 0);     // length (no extra data)
      wire::wr16_le(rep.data() + 8, 2);     // server_minor_version (2.2)
      (void)ctx.reply().sendReplyRaw(rep.data(), rep.size());
      return;
    }

    // ---- minor 1: XTestCompareCursor (reply-bearing) ----
    case 1: {
      uint32_t window = br.readU32();
      uint32_t cursor = br.readU32();
      (void)window; (void)cursor;
      br.skip(br.remaining());

      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 1;                            // same = true
        wire::wr32_le(rep.data() + 4, 0);     // length (no extra data)
      });
      return;
    }

    // ---- minor 2: XTestFakeInput (void — no reply) ----
    case 2: {
      // Request: CARD8 type, CARD8 detail, pad16, CARD32 time,
      //          CARD32 root, pad32, pad32, CARD16 rootX, CARD16 rootY
      // Silently consume for now — synthesized events not yet routed.
      br.skip(br.remaining());
      return;
    }

    // ---- minor 3: XTestGrabControl (void — no reply) ----
    case 3: {
      // Request: BOOL impervious, pad*3
      // Controls whether XTEST events bypass grabs. Silently consume.
      br.skip(br.remaining());
      return;
    }

    default:
      break;
    }
    // Unhandled XTEST minor — send BadRequest
    { char buf[128]; snprintf(buf, sizeof(buf), "[XTEST] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
    br.skip(br.remaining());
    ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
    return;
  }

  // -------------------------------------------------------------------
  // Composite — major opcode 143
  // GTK3 relies on Composite for proper widget compositing.  Without it,
  // portal-GTK dialogs render with inverted colors (broken fallback path).
  // Stub: advertise version 0.4, silently consume all sub-opcodes.
  // -------------------------------------------------------------------
  if (major == ext::kCOMPOSITE) {
    switch (minor) {

    // ---- minor 0: CompositeQueryVersion (reply-bearing) ----
    case 0: {
      // Request: CARD32 client_major, CARD32 client_minor
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 8, 0);   // major version = 0
        wire::wr32_le(rep.data() + 12, 4);  // minor version = 4
      });
      return;
    }

    // ---- minor 1: CompositeRedirectWindow (void) ----
    // ---- minor 2: CompositeRedirectSubwindows (void) ----
    // ---- minor 3: CompositeUnredirectWindow (void) ----
    // ---- minor 4: CompositeUnredirectSubwindows (void) ----
    // ---- minor 5: CompositeCreateRegionFromBorderClip (void) ----
    // ---- minor 6: CompositeNameWindowPixmap (void) ----
    case 1: case 2: case 3: case 4: case 5: case 6:
      br.skip(br.remaining());
      return;

    // ---- minor 7: CompositeGetOverlayWindow (reply-bearing) ----
    case 7: {
      // Request: CARD32 window
      // Reply: CARD32 overlay_window
      // Return the root window as the overlay — matches typical WM behavior
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 8, 1);  // overlay_win = root (XID 1)
      });
      return;
    }

    // ---- minor 8: CompositeReleaseOverlayWindow (void) ----
    case 8:
      br.skip(br.remaining());
      return;

    default:
      break;
    }
    // Unhandled Composite minor
    br.skip(br.remaining());
    ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
    return;
  }

  // -------------------------------------------------------------------
  // DAMAGE — major opcode 144
  // Often paired with Composite.  Tracks drawable content changes.
  // Stub: advertise version 1.1, silently consume all sub-opcodes.
  // -------------------------------------------------------------------
  if (major == ext::kDAMAGE) {
    switch (minor) {

    // ---- minor 0: DamageQueryVersion (reply-bearing) ----
    case 0: {
      // Request: CARD32 client_major, CARD32 client_minor
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 8, 1);   // major version = 1
        wire::wr32_le(rep.data() + 12, 1);  // minor version = 1
      });
      return;
    }

    // ---- minor 1: DamageCreate (void) ----
    // ---- minor 2: DamageDestroy (void) ----
    // ---- minor 3: DamageSubtract (void) ----
    // ---- minor 4: DamageAdd (void) ----
    case 1: case 2: case 3: case 4:
      br.skip(br.remaining());
      return;

    default:
      break;
    }
    // Unhandled DAMAGE minor
    br.skip(br.remaining());
    ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
    return;
  }

  // Fallthrough — unknown extension major (not dispatched by ExtDispatcher)
  { char buf[128]; snprintf(buf, sizeof(buf), "[ExtensionOps] unknown major=%u minor=%u seq=%u\n", (unsigned)major, (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
  br.skip(br.remaining());
}

} // namespace x11
