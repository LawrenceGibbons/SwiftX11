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

#include "Ops/ExtensionOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/WindowTable.hpp"
#include "Core/WindowView.hpp"
#include "Core/PixmapTable.hpp"
#include "Core/ShapeRegion.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Utils/ByteReader.hpp"
#include "Utils/WireLE.hpp"
#include "Core/X11ExtOpcodes.hpp"

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
      fprintf(stderr, "[XFIXES] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq);
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

#ifndef NDEBUG
      fprintf(stderr, "[SHAPE] ShapeRectangles wid=0x%X kind=%u op=%u nrects=%zu host=0x%X\n",
              wid, kind, op, nrects, host);
#endif
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
#ifndef NDEBUG
        fprintf(stderr, "[SHAPE] ShapeMask lookup pixmap=0x%X found=%d depth=%u w=%u h=%u bits=%p stride=%u\n",
                pixmap_id, found, pm.depth, pm.w, pm.h, (const void*)pm.bits, pm.stride_bytes);
#endif
        if (found && pm.depth == 1 && pm.bits) {
          if (op != 0) {
            if (kind == 0)      region = ctx.windows().shapeBounding(wid);
            else if (kind == 2) region = ctx.windows().shapeInput(wid);
          }
          region.setFromBitmap(pm.bits, pm.w, pm.h, (int)pm.stride_bytes,
                               x_off, y_off, ww, wh, op);
#ifndef NDEBUG
          fprintf(stderr, "[SHAPE] ShapeMask result: shaped=%d nrects=%zu\n",
                  region.shaped, region.rects.size());
#endif
        } else {
          // Unknown pixmap — treat as unshaped
#ifndef NDEBUG
          fprintf(stderr, "[SHAPE] ShapeMask FAILED: pixmap not found or wrong depth/bits\n");
#endif
          region.reset();
        }
      }

      if (kind == 0)      ctx.windows().setShapeBounding(wid, std::move(region));
      else if (kind == 1) ctx.windows().setShapeClip(wid, std::move(region));
      else if (kind == 2) ctx.windows().setShapeInput(wid, std::move(region));

      uint32_t host = ctx.windows().topLevelAncestorOf(wid);
      if (host == 0) host = wid;
      x11_ui_push_shape_changed(host);

#ifndef NDEBUG
      fprintf(stderr, "[SHAPE] ShapeMask wid=0x%X kind=%u op=%u pixmap=0x%X host=0x%X\n",
              wid, kind, op, pixmap_id, host);
#endif
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
      fprintf(stderr, "[SHAPE] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq);
      br.skip(br.remaining());
      // Send error to prevent XCB sequence desync if sub-opcode was reply-bearing.
      ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
      return;
    }
  }

  // -------------------------------------------------------------------
  // RANDR — single-screen stubs (version 1.3)
  //
  // Fake resource IDs for our single output/crtc/mode:
  //   Output  = 0x00100000
  //   CRTC    = 0x00100001
  //   Mode    = 0x00100010
  // Screen dimensions hardcoded to 1920×1080 (matches Xinerama stub).
  // -------------------------------------------------------------------
  static constexpr uint32_t kRR_Output = 0x00100000;
  static constexpr uint32_t kRR_Crtc   = 0x00100001;
  static constexpr uint32_t kRR_ModeId = 0x00100010;
  static constexpr uint16_t kRR_W      = 1920;
  static constexpr uint16_t kRR_H      = 1080;
  static constexpr uint16_t kRR_Wmm    = 508;  // ~25.4 * 1920 / 96
  static constexpr uint16_t kRR_Hmm    = 285;  // ~25.4 * 1080 / 96

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
    case 19: // RRGetScreenResourcesCurrent (1.3)
    {
      // Reply: 1 CRTC, 1 output, 1 mode
      br.skip(br.remaining());

      // Mode name "1920x1080" (9 chars)
      static const char modeName[] = "1920x1080";
      static constexpr uint16_t modeNameLen = 9;

      // Build ModeInfo (32 bytes)
      uint8_t modeInfo[32] = {};
      wire::wr32_le(modeInfo + 0,  kRR_ModeId);
      wire::wr16_le(modeInfo + 4,  kRR_W);       // width
      wire::wr16_le(modeInfo + 6,  kRR_H);       // height
      wire::wr32_le(modeInfo + 8,  148500000);    // dotClock (148.5 MHz, 1080p60)
      wire::wr16_le(modeInfo + 12, 2008);         // hSyncStart
      wire::wr16_le(modeInfo + 14, 2052);         // hSyncEnd
      wire::wr16_le(modeInfo + 16, 2200);         // hTotal
      wire::wr16_le(modeInfo + 18, 0);            // hSkew
      wire::wr16_le(modeInfo + 20, 1084);         // vSyncStart
      wire::wr16_le(modeInfo + 22, 1089);         // vSyncEnd
      wire::wr16_le(modeInfo + 24, 1125);         // vTotal
      wire::wr16_le(modeInfo + 26, modeNameLen);  // nameLen
      wire::wr32_le(modeInfo + 28, 0);            // modeFlags

      // Payload: crtcIds(4) + outputIds(4) + ModeInfo(32) + name(9) + pad(3) = 52 bytes
      uint8_t payload[52] = {};
      wire::wr32_le(payload + 0, kRR_Crtc);       // crtc[0]
      wire::wr32_le(payload + 4, kRR_Output);     // output[0]
      std::memcpy(payload + 8, modeInfo, 32);      // modes[0]
      std::memcpy(payload + 40, modeName, modeNameLen); // name bytes

      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 13);  // length = 52/4
        wire::wr32_le(rep.data() + 8,  0);  // timestamp
        wire::wr32_le(rep.data() + 12, 0);  // configTimestamp
        wire::wr16_le(rep.data() + 16, 1);  // num_crtcs
        wire::wr16_le(rep.data() + 18, 1);  // num_outputs
        wire::wr16_le(rep.data() + 20, 1);  // num_modes
        wire::wr16_le(rep.data() + 22, modeNameLen); // names_len
      });
      ctx.reply().sendBytes(payload, sizeof(payload));
      return;
    }

    case 9: {
      // RRGetOutputInfo — single connected output
      br.skip(br.remaining());

      // Output name "Virtual-1" (9 chars)
      static const char outputName[] = "Virtual-1";
      static constexpr uint16_t nameLen = 9;

      // Payload after 32-byte header:
      //   nClones(2) + nameLength(2) + crtcs(4) + modes(4) + clones(0) + name(9) + pad(3) = 24
      uint8_t payload[24] = {};
      wire::wr16_le(payload + 0, 0);          // num_clones
      wire::wr16_le(payload + 2, nameLen);    // name_length
      wire::wr32_le(payload + 4, kRR_Crtc);  // crtcs[0]
      wire::wr32_le(payload + 8, kRR_ModeId);// modes[0]
      // no clones
      std::memcpy(payload + 12, outputName, nameLen); // name

      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 0; // status = RRSetConfigSuccess
        wire::wr32_le(rep.data() + 4, 6);       // length = 24/4
        wire::wr32_le(rep.data() + 8, 0);       // timestamp
        wire::wr32_le(rep.data() + 12, kRR_Crtc);  // crtc
        wire::wr32_le(rep.data() + 16, kRR_Wmm);   // mm_width
        wire::wr32_le(rep.data() + 20, kRR_Hmm);   // mm_height
        rep[24] = 0; // connection = Connected
        rep[25] = 0; // subpixel_order = Unknown
        wire::wr16_le(rep.data() + 26, 1); // num_crtcs
        wire::wr16_le(rep.data() + 28, 1); // num_modes
        wire::wr16_le(rep.data() + 30, 1); // num_preferred
      });
      ctx.reply().sendBytes(payload, sizeof(payload));
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

    case 13: {
      // RRGetCrtcInfo — single CRTC at 0,0
      br.skip(br.remaining());

      // Payload: outputs(4) + possible_outputs(4) = 8 bytes
      uint8_t payload[8] = {};
      wire::wr32_le(payload + 0, kRR_Output); // outputs[0]
      wire::wr32_le(payload + 4, kRR_Output); // possible[0]

      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 0; // status
        wire::wr32_le(rep.data() + 4, 2);        // length = 8/4
        wire::wr32_le(rep.data() + 8, 0);        // timestamp
        wire::wr16_le(rep.data() + 12, 0);       // x
        wire::wr16_le(rep.data() + 14, 0);       // y
        wire::wr16_le(rep.data() + 16, kRR_W);   // width
        wire::wr16_le(rep.data() + 18, kRR_H);   // height
        wire::wr32_le(rep.data() + 20, kRR_ModeId); // mode
        wire::wr16_le(rep.data() + 24, 1);       // rotation = Rotate_0
        wire::wr16_le(rep.data() + 26, 1);       // rotations = Rotate_0
        wire::wr16_le(rep.data() + 28, 1);       // num_outputs
        wire::wr16_le(rep.data() + 30, 1);       // num_possible
      });
      ctx.reply().sendBytes(payload, sizeof(payload));
      return;
    }

    case 14: {
      // RRSetCrtcConfig — reply success
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 0; // status = RRSetConfigSuccess
        wire::wr32_le(rep.data() + 4, 0);
        wire::wr32_le(rep.data() + 8, 0); // timestamp
      });
      return;
    }

    case 15: {
      // RRGetCrtcGammaSize — reply size=0
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);
        wire::wr16_le(rep.data() + 8, 0); // size
      });
      return;
    }

    case 31: {
      // RRGetOutputPrimary — reply with our output XID
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);
        wire::wr32_le(rep.data() + 8, kRR_Output); // output
      });
      return;
    }

    default:
      fprintf(stderr, "[RANDR] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq);
      br.skip(br.remaining());
      ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
      return;
    }
  }

  // -------------------------------------------------------------------
  // Xinerama — minor 0 = QueryVersion, minor 4 = IsActive, minor 5 = QueryScreens
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
      // Reply: state=1 (active, single screen)
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0); // length
        wire::wr32_le(rep.data() + 8, 1); // state = active
      });
      return;
    }
    if (minor == 5) {
      // XineramaQueryScreens — no request body
      br.skip(br.remaining());
      // Reply: 1 screen. Payload = 1 × XineramaScreenInfo (8 bytes)
      // XineramaScreenInfo: INT16 x, INT16 y, CARD16 w, CARD16 h
      uint8_t payload[8] = {};
      wire::wr16_le(payload + 0, 0);    // x = 0
      wire::wr16_le(payload + 2, 0);    // y = 0
      wire::wr16_le(payload + 4, 1920); // w (default)
      wire::wr16_le(payload + 6, 1080); // h (default)

      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 2); // length = 8 bytes / 4 = 2 words
        wire::wr32_le(rep.data() + 8, 1); // number = 1 screen
      });
      ctx.reply().sendBytes(payload, sizeof(payload));
      return;
    }
    fprintf(stderr, "[Xinerama] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq);
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
    fprintf(stderr, "[GE] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq);
    br.skip(br.remaining());
    // Send error to prevent XCB sequence desync if sub-opcode was reply-bearing.
    ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, major);
    return;
  }

  // Fallthrough
#ifndef NDEBUG
  fprintf(stderr, "[ExtensionOps] unknown major=%u minor=%u seq=%u\n",
          (unsigned)major, (unsigned)minor, (unsigned)seq);
#endif
  br.skip(br.remaining());
}

} // namespace x11
