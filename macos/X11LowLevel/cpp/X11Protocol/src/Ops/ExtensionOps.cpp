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
#include "Ops/SelectionOps.hpp"
#include "Extensions/XKBOps.hpp"   // XKEYBOARD (major 145)
#include "Core/XProtoContext.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/WindowTable.hpp"
#include "Core/WindowView.hpp"
#include "Core/InputRouting.hpp"   // pickDeepestMappedWindowAtHostPoint (XIQueryPointer)
#include "Core/CursorRouting.hpp"  // maybeApplyCursor (XIChangeCursor)
#include "Core/GrabTable.hpp"      // tryPointerGrab/clearPointerGrab (XIGrabDevice)
#include "Core/XI2EventMask.hpp"   // device ids (XIGrabDevice routes by device class)
#include "Core/timestamp.hpp"      // x11_now_ms_monotonic (grab time checks)
#include "Utils/GrabChoreography.hpp" // grab activation/deactivation choreography
#include "Core/XProtoServer.hpp"   // eventOps() for grab-activation crossings
#include "Ops/EventOps.hpp"

// Bridge accessor (defined in XProtoServerBridge.cpp) — lets the XI2 grab
// handlers emit grab/ungrab crossing events via EventOps.
extern "C" x11::XProtoServer* x11_proto_bridge_get_server(void);
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
#include "Core/AtomTable.hpp"      // XI2 button/axis label atoms (XIQueryDevice)
#include "Core/X11Modifiers.hpp"   // toX11State (XIQueryPointer mods)
#include "Utils/FocusEvents.hpp"   // XISetFocus → DoFocusEvents

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
  reg.registerMajor(ext::kXKB,       &ExtensionOps::onMajor, this);
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
  // XKEYBOARD — major opcode 145 (Extensions/XKBOps.cpp)
  // -------------------------------------------------------------------
  if (major == ext::kXKB) {
    XKBOps::dispatch(ctx, dc);
    return;
  }

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
    case 2: { // XFixesSelectSelectionInput (M4)
      // Request body: Window window, Atom selection, CARD32 eventMask.
      // Record the subscription so SetSelectionOwner can fire
      // XFixesSelectionNotify(SetSelectionOwnerNotify) back to this client.
      if (br.remaining() < 12) { br.skip(br.remaining()); return; }
      const uint32_t window    = br.readU32();
      const uint32_t selection = br.readU32();
      const uint32_t eventMask = br.readU32();
      br.skip(br.remaining());
      SelectionOps::xfixesSelectSelectionInput(ctx.transport().clientFd(),
                                               window, selection, eventMask);
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
#ifdef X11_TRACE_VERBOSE
    // Per-request trace is verbose-only: GTK3 issues XIQueryPointer at a high
    // rate (L10 in docs/XI2_XORG_COMPARISON.md).
    { char buf[128]; snprintf(buf, sizeof(buf), "[XInput2] fd=%d minor=%u seq=%u\n",
        ctx.transport().clientFd(), (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
#endif
    // Phase E (M13, L11): XI errors carry the minor opcode and the device
    // errors use the extension's error base (BadDevice = first_error + 0,
    // Xi/extinit.c:1065); every reply carries the minor in RepType (byte 1).
    const uint8_t kBadDevice = ext::kXInput_FirstError;
    auto xiError = [&](uint8_t code, uint32_t value) {
      br.skip(br.remaining());
      (void)ctx.transport().sendErrorExt(code, seq, value, minor, ext::kXInput2);
    };
    auto isKeyboardDev = [](uint16_t d) {
      return d == x11::xi2::kVirtualCoreKeyboard || d == x11::xi2::kXTESTKeyboard ||
             d == x11::xi2::kRealKeyboard;
    };
    auto fp1616 = [](int32_t v) { return (uint32_t)((int64_t)v * 65536); };

    switch (minor) {

    // ---- minor 1: GetExtensionVersion (XI1 legacy — reply-bearing) ----
    case 1: {
      // libXi sends this before XIQueryVersion.
      // Request: CARD16 name_len, pad16, then name bytes.
      br.skip(br.remaining());
      // Reply: XIVersion = 2.2, present (Xi/getvers.c:101-109) — M13.
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 1;                             // RepType = X_GetExtensionVersion
        wire::wr32_le(rep.data() + 4, 0);     // length
        wire::wr16_le(rep.data() + 8, 2);     // server_major
        wire::wr16_le(rep.data() + 10, 2);    // server_minor
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
      // xorg Xi/listdev.c: every device (ShouldSkipDevice skips only masters
      // other than VCP/VCK, :305-318); `use` per :174-183 — IsXPointer 0 /
      // IsXKeyboard 1 for the masters, IsXExtensionPointer 4 /
      // IsXExtensionKeyboard 3 for slaves (XI.h:189-193; the old table had 3
      // and 4 swapped); `attached` = the master's id for a slave.  Phase E, L1.
      const XI1Dev devs[] = {
        { 2, 0, 0, "Virtual core pointer",         2 },  // ButtonInfo + ValuatorInfo
        { 3, 1, 0, "Virtual core keyboard",        1 },  // KeyInfo
        { 4, 4, 2, "Virtual core XTEST pointer",   2 },
        { 5, 3, 3, "Virtual core XTEST keyboard",  1 },
        { 6, 4, 2, "SwiftX11 pointer",             2 },
        { 7, 3, 3, "SwiftX11 keyboard",            1 },
      };
      constexpr uint8_t ndevices = 6;

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
        if (d.use == 0 || d.use == 4) { // pointer devices
          // xButtonInfo: class=1, length=4, num_buttons=10 (core pointer)
          push8(1); push8(4); push16(10);
          // xValuatorInfo: class=2, length=8+2*12, num_axes=2, mode=0 (Relative),
          // motion_buffer_size=0, then one xAxisInfo per axis (resolution, min,
          // max; NO_AXIS_LIMITS = -1) — Xi/listdev.c CopySwapValuatorClass.
          push8(2); push8(32); push8(2); push8(0); push32(0);
          for (int a = 0; a < 2; a++) { push32(0); push32(0xFFFFFFFFu); push32(0xFFFFFFFFu); }
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

    // ---- XI1 minors 3–39: numbered per xorg's dispatch vector
    // (Xi/extinit.c:186-227) — Phase E, M12.  The old table had GrabDevice
    // at 7 (= GetSelectedExtensionEvents), GetDeviceFocus at 10 (=
    // GetDeviceMotionEvents) and QueryDeviceState at 24 (= GetDeviceKeyMapping),
    // so thirteen reply-bearing minors were consumed without a reply.

    // ---- minor 13: GrabDevice (XI1 reply-bearing) ----
    case 13: {
      // Request: CARD32 grab_window, CARD32 time, CARD16 num_classes,
      //          CARD8 this_device_mode, CARD8 other_device_mode,
      //          BOOL owner_events, CARD8 deviceid, CARD16 pad,
      //          then class list.
      br.skip(br.remaining());
      // xGrabDeviceReply: status(8) = Success.
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 13;                         // RepType
        wire::wr32_le(rep.data() + 4, 0);  // length = 0
        rep[8] = 0;                          // status = Success
      });
      return;
    }

    // ---- minor 20: GetDeviceFocus (XI1 reply-bearing) ----
    case 20: {
      // Request: CARD8 deviceid, pad*3
      br.skip(br.remaining());
      // xGetDeviceFocusReply: focus(8-11) None / PointerRoot(1) / window,
      // time(12-15), revertTo(16) — same mapping as XIGetFocus.
      const uint32_t f = ctx.input().focus_xid;
      const uint32_t focusWin = (f == 0) ? 0u : (f == x11::kPointerRootFocus) ? 1u : f;
      const uint8_t revert = ctx.input().focus_revert_to;   // 0 None, 1 PointerRoot, 2 Parent
      (void)ctx.reply().sendReply32(seq, [focusWin, revert](std::array<uint8_t, 32>& rep) {
        rep[1] = 20;                                // RepType
        wire::wr32_le(rep.data() + 4, 0);         // length = 0
        wire::wr32_le(rep.data() + 8, focusWin);  // focus
        wire::wr32_le(rep.data() + 12, 0);        // time = CurrentTime
        rep[16] = revert;                           // revertTo
      });
      return;
    }

    // ---- minor 30: QueryDeviceState (XI1 reply-bearing) ----
    case 30: {
      // Request: CARD8 deviceid, pad*3
      br.skip(br.remaining());
      // xQueryDeviceStateReply: num_classes(8) = 0, no trailing class data.
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 30;                         // RepType
        wire::wr32_le(rep.data() + 4, 0);  // length = 0
        rep[8] = 0;                          // num_classes = 0
      });
      return;
    }

    // ---- other reply-bearing XI1 minors: BadRequest keeps XCB's sequence aligned ----
    case 3:  // OpenDevice
    case 5:  // SetDeviceMode
    case 7:  // GetSelectedExtensionEvents
    case 9:  // GetDeviceDontPropagateList
    case 10: // GetDeviceMotionEvents
    case 11: // ChangeKeyboardDevice
    case 12: // ChangePointerDevice
    case 22: // GetFeedbackControl
    case 24: // GetDeviceKeyMapping
    case 26: // GetDeviceModifierMapping
    case 27: // SetDeviceModifierMapping
    case 28: // GetDeviceButtonMapping
    case 29: // SetDeviceButtonMapping
    case 33: // SetDeviceValuators
    case 34: // GetDeviceControl
    case 35: // ChangeDeviceControl
    case 36: // ListDeviceProperties
    case 39: // GetDeviceProperty
      xiError(x11::error::BadRequest, 0);
      return;

    // ---- void XI1 minors: consume ----
    case 4:  // CloseDevice
    case 6:  // SelectExtensionEvent
    case 8:  // ChangeDeviceDontPropagateList
    case 14: // UngrabDevice
    case 15: // GrabDeviceKey
    case 16: // UngrabDeviceKey
    case 17: // GrabDeviceButton
    case 18: // UngrabDeviceButton
    case 19: // AllowDeviceEvents
    case 21: // SetDeviceFocus
    case 23: // ChangeFeedbackControl
    case 25: // ChangeDeviceKeyMapping
    case 31: // SendExtensionEvent
    case 32: // DeviceBell
    case 37: // ChangeDeviceProperty
    case 38: // DeleteDeviceProperty
      br.skip(br.remaining());
      return;

    // ---- minor 40: XIQueryPointer (reply-bearing) ----
    // Real pointer state (mirrors core QueryPointer + ProcXIQueryPointer):
    // GTK's file dialog polls this thousands of times for hit-testing, so the
    // old hardcoded (0,0)/no-child reply broke hover and double-click.
    case 40: {
      uint32_t qwin = 0; uint16_t deviceid = 0;
      if (br.remaining() >= 6) { qwin = br.readU32(); deviceid = br.readU16(); }
      br.skip(br.remaining());
      // Xi/xiquerypointer.c:100-117 (Phase E, M14): unknown device or a
      // keyboard (no valuators) → BadDevice; win None or unknown → BadWindow.
      if (!x11::xi2::isKnownDevice(deviceid) || isKeyboardDev(deviceid)) {
        xiError(kBadDevice, deviceid);
        return;
      }
      const bool qIsRoot = (qwin == x11::kRootWindowXid);
      if (qwin == 0 || (!qIsRoot && !ctx.window(qwin))) {
        xiError(x11::error::BadWindow, qwin);
        return;
      }

      const auto& in = ctx.input();
      const int32_t root_x = in.root_x_u, root_y = in.root_y_u;
      const uint32_t host = in.last_xid;

      // :158-174 — one screen, so same_screen is always True and win_x/win_y
      // are the sprite position relative to the queried window's root origin
      // wherever the pointer is.  (The old reply said same_screen=0 with zero
      // coords when the pointer was over another host; libXi returns that
      // byte as its Bool result and GDK read it as failure, keeping stale
      // coordinates and modifiers.)  `child` is the direct child of the
      // queried window on the sprite path (t->parent == pWin), else None.
      int32_t win_x = root_x, win_y = root_y;
      uint32_t child = 0;
      if (qIsRoot) {
        // Toplevel under the pointer — only while the pointer really is in
        // it (last_xid lingers after the pointer leaves; the cached local
        // coords follow the real position, so the pick fails cleanly).
        child = (host && pickDeepestMappedWindowAtHostPoint(ctx, host, in.win_x_u, in.win_y_u)) ? host : 0;
      } else {
        int32_t ox = 0, oy = 0;
        uint32_t cur = qwin;
        for (int hop = 0; cur && cur != x11::kRootWindowXid && hop < 256; hop++) {
          WindowView cv{};
          if (!ctx.windows().snapshot(cur, cv)) break;
          ox += (int32_t)cv.x + (int32_t)cv.border_width;
          oy += (int32_t)cv.y + (int32_t)cv.border_width;
          cur = cv.parent_xid;
        }
        win_x = root_x - ox; win_y = root_y - oy;
        if (host && ctx.windows().topLevelAncestorOf(qwin) == host) {
          uint32_t t = pickDeepestMappedWindowAtHostPoint(ctx, host, in.win_x_u, in.win_y_u);
          if (!t) t = host;
          for (int hop = 0; t && t != x11::kRootWindowXid && hop < 64; hop++) {
            WindowView tv{};
            if (!ctx.windows().snapshot(t, tv)) break;
            if (tv.parent_xid == qwin) { child = t; break; }
            t = tv.parent_xid;
          }
        }
      }

      // Held buttons in one mask word (buttons_len = bytes_to_int32(
      // bits_to_bytes(10)) = 1, :139-141); modifier state split as XKB keeps
      // it (Caps Lock locked, the rest base; :119-127).
      uint32_t btnmask = 0;
      for (int i = 0; i < 10; i++) if (in.buttons & (1u << i)) btnmask |= (1u << (i + 1));
      const uint32_t x11mods = x11::input::toX11State(0, in.mods) & 0xFFu;
      const uint32_t locked  = x11mods & 0x02u;

      std::array<uint8_t, 60> rep{};
      rep[0] = 1;                                    // Reply
      rep[1] = 40;                                   // RepType = X_XIQueryPointer
      wire::wr16_le(rep.data() + 2, seq);
      wire::wr32_le(rep.data() + 4, 7);              // length = 6 + buttons_len
      wire::wr32_le(rep.data() + 8, x11::kRootWindowXid);  // root
      wire::wr32_le(rep.data() + 12, child);         // child
      wire::wr32_le(rep.data() + 16, fp1616(root_x));
      wire::wr32_le(rep.data() + 20, fp1616(root_y));
      wire::wr32_le(rep.data() + 24, fp1616(win_x));
      wire::wr32_le(rep.data() + 28, fp1616(win_y));
      rep[32] = 1;                                   // same_screen
      rep[33] = 0;                                   // pad
      wire::wr16_le(rep.data() + 34, 1);             // buttons_len = 1
      wire::wr32_le(rep.data() + 36, x11mods & ~locked); // mods.base
      wire::wr32_le(rep.data() + 40, 0);                 // mods.latched
      wire::wr32_le(rep.data() + 44, locked);            // mods.locked
      wire::wr32_le(rep.data() + 48, x11mods);           // mods.effective
      // group (52-55) zero
      wire::wr32_le(rep.data() + 56, btnmask);       // button mask
      (void)ctx.reply().sendReplyRaw(rep.data(), rep.size());
      return;
    }

    // ---- minor 41, 43, 44: XI2 void stubs ----
    case 41: // XIWarpPointer
    case 43: // XIChangeHierarchy
    case 44: // XISetClientPointer
      br.skip(br.remaining());
      return;

    // ---- minor 42: XIChangeCursor (void) ----
    // xorg Xi/xichangecursor.c: look up window and cursor, then
    // ChangeWindowDeviceCursor() — the per-device twin of
    // ChangeWindowAttributes(CWCursor).  GTK3 sets every window cursor via
    // XIDefineCursor once XI2 is present, so the old no-op left portal-GTK
    // dialogs with a permanent arrow (M6 in docs/XI2_XORG_COMPARISON.md).
    // One pointer here, so deviceid is not consulted.
    case 42: {
      if (br.remaining() < 12) { br.skip(br.remaining()); return; }
      const uint32_t win    = br.readU32();
      const uint32_t cursor = br.readU32();   // 0 = None (inherit)
      br.skip(br.remaining());                 // deviceid(2) + pad(2)
      if (win == x11::kRootWindowXid) return;                    // root: nothing to route to
      x11::WindowView wv{};
      if (!ctx.windows().snapshot(win, wv)) {
        xiError(x11::error::BadWindow, win);
        return;
      }
      // Same path as the core CWCursor branch (WindowAttrOps.cpp).
      ctx.windows().setCursor(win, cursor);
      const uint32_t host = ctx.windows().topLevelAncestorOf(win);
      const bool pointerInThisHost = (host != 0) &&
          (ctx.input().last_xid == host || ctx.input().focus_host == host);
      if (pointerInThisHost) {
        uint32_t underNow = x11::pickDeepestMappedWindowAtHostPoint(ctx, host,
                                                                    ctx.input().win_x_u,
                                                                    ctx.input().win_y_u);
        if (!underNow) underNow = host;
        x11::maybeApplyCursor(ctx, host, ctx.input().routePointer(underNow));
      }
      return;
    }

    case 45: { // XIGetClientPointer (reply-bearing)
      // xXIGetClientPointerReply (XI2proto.h): set @8, pad @9, deviceid @10.
      // Was written to bytes 1 and 8 (the RepType and `set` slots), so GDK
      // decoded deviceid=0 (M2 in docs/XI2_XORG_COMPARISON.md).
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 45;                          // RepType
        wire::wr32_le(rep.data() + 4, 0);   // length
        rep[8] = 1;                           // set = True
        wire::wr16_le(rep.data() + 10, 2);   // deviceid = Virtual core pointer
      });
      return;
    }

    // ---- minor 46: XISelectEvents (void — no reply) ----
    case 46: {
      // XISelectEvents: CARD32 window, CARD16 num_masks, pad16
      //   then per mask: CARD16 deviceid, CARD16 mask_len, mask_len*4 bytes
      // Phase C (M4): one selection per (client, device spec) per window, as
      // xorg XISetEventMask; validated whole before anything is applied
      // (Xi/xiselectev.c:145-322).
      const uint8_t xiMinor = 46;
      auto fail = [&](uint8_t code, uint32_t value) {
        br.skip(br.remaining());
        (void)ctx.transport().sendErrorExt(code, seq, value, xiMinor, ext::kXInput2);
      };
      if (br.remaining() < 8) { fail(x11::error::BadLength, 0); return; }
      const uint32_t window    = br.readU32();
      const uint16_t num_masks = br.readU16();
      br.skip(2); // pad
      if (num_masks == 0) { fail(x11::error::BadValue, 0); return; }
      const bool isRoot = (window == x11::kRootWindowXid);   // raw events are root-only (below)
      if (!ctx.window(window)) { fail(x11::error::BadWindow, window); return; }   // R1: root is a window

      struct Sel { uint16_t dev; uint32_t mask; };
      std::vector<Sel> sels;
      for (uint16_t i = 0; i < num_masks; i++) {
        if (br.remaining() < 4) { fail(x11::error::BadLength, 0); return; }
        const uint16_t deviceid = br.readU16();
        const uint16_t mask_len = br.readU16(); // in 4-byte units
        if (br.remaining() < (size_t)mask_len * 4u) { fail(x11::error::BadLength, 0); return; }
        uint32_t mask = 0;
        for (uint16_t j = 0; j < mask_len; j++) {
          const uint32_t word = br.readU32();
          if (j == 0) mask = word;  // event types 0-31 live in word 0
        }
        // Device spec: XIAllDevices, XIAllMasterDevices or an id we advertise
        // (dixLookupDevice fails → BadDevice, Xi/xiselectev.c:173-178).
        if (deviceid != xi2::kAllDevices && deviceid != xi2::kAllMasterDevices &&
            !xi2::isKnownDevice(deviceid)) { fail(kBadDevice, deviceid); return; }
        // HierarchyChanged only for XIAllDevices; raw events only on root.
        if (deviceid != xi2::kAllDevices && (mask & xi2::kHierarchyChangedMask)) {
          fail(x11::error::BadValue, 11); return;
        }
        if (!isRoot && (mask & xi2::kRootOnlyMask)) { fail(x11::error::BadValue, 13); return; }
        sels.push_back({deviceid, mask});
      }
      br.skip(br.remaining()); // trailing padding

      const int fd = ctx.transport().clientFd();
      // R1 Phase 3 (A5): root's selections are stored like any window's —
      // xorg keeps one InputClients list per window, root included.
      for (const Sel& s : sels) ctx.windows().setClientXI2Mask(window, fd, s.dev, s.mask);
      return;
    }

    // ---- minor 47: XIQueryVersion (reply-bearing) ----
    // xorg Xi/xiqueryversion.c:66-115 (Phase E, M13): BadValue below 2.0;
    // the reply is min(server 2.2, client); the first answer is remembered
    // per client, and a later request may only raise it when both sides are
    // at 2.2 or above (Peter's no-more-breaking promise) — asking for less
    // than the remembered version is BadValue, asking for more (below 2.2)
    // gets the remembered one.
    case 47: {
      uint16_t cmaj = 0, cmin = 0;
      if (br.remaining() >= 4) { cmaj = br.readU16(); cmin = br.readU16(); }
      br.skip(br.remaining());
      if (cmaj < 2) { xiError(x11::error::BadValue, cmaj); return; }
      auto cmp = [](uint16_t a1, uint16_t b1, uint16_t a2, uint16_t b2) -> int {
        if (a1 != a2) return a1 < a2 ? -1 : 1;
        if (b1 != b2) return b1 < b2 ? -1 : 1;
        return 0;
      };
      uint16_t maj = 2, min = 2;                              // XIVersion
      if (cmp(2, 2, cmaj, cmin) > 0) { maj = cmaj; min = cmin; }
      XClient* cl = ctx.hasClient() ? ctx.client() : nullptr;
      if (cl) {
        if (cl->xi2Major()) {
          if (cmp(maj, min, 2, 2) >= 0 && cmp(cl->xi2Major(), cl->xi2Minor(), 2, 2) >= 0) {
            if (cmp(maj, min, cl->xi2Major(), cl->xi2Minor()) > 0) cl->setXI2Version(maj, min);
          } else {
            if (cmp(maj, min, cl->xi2Major(), cl->xi2Minor()) < 0) {
              xiError(x11::error::BadValue, cmaj);
              return;
            }
            maj = cl->xi2Major(); min = cl->xi2Minor();
          }
        } else {
          cl->setXI2Version(maj, min);
        }
      }
      (void)ctx.reply().sendReply32(seq, [maj, min](std::array<uint8_t, 32>& rep) {
        rep[1] = 47;                            // RepType
        wire::wr32_le(rep.data() + 4, 0);     // length
        wire::wr16_le(rep.data() + 8, maj);   // major_version
        wire::wr16_le(rep.data() + 10, min);  // minor_version
      });
      return;
    }

    // ---- minor 48: XIQueryDevice (reply-bearing with payload) ----
    case 48: {
      // Request: CARD16 deviceid (0=XIAllDevices, 1=XIAllMasterDevices, or specific)
      const uint16_t requested_device = br.remaining() >= 2 ? br.readU16() : 0;
      br.skip(br.remaining());
      // Xi/xiquerydevice.c:81-87: a specific id must name a device → BadDevice.
      if (requested_device != 0 && requested_device != 1 &&
          !x11::xi2::isKnownDevice(requested_device)) {
        xiError(kBadDevice, requested_device);
        return;
      }

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
        // Real (physical) slaves — genuine user input is sourced from these, not
        // the XTEST slaves, so Chromium/GTK treat it as real input (menus/dialogs).
        // Mirrors nxagent's "nxagent mouse"/"nxagent keyboard" slaves.
        { 6, 3, 2, "SwiftX11 pointer",             true,  false },  // SlavePointer  attached to master 2
        { 7, 4, 3, "SwiftX11 keyboard",            false, true  },  // SlaveKeyboard attached to master 3
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
        if (dev->has_buttons) num_classes += 3; // ButtonClass + 2 ValuatorClass (X,Y)
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

        // ButtonClass for pointer devices — xorg CorePointerProc
        // (dix/devices.c:644-668): 10 buttons, the first seven labelled
        // (Button Left … Button Horiz Wheel Right, the rest None), state =
        // the held buttons (ListButtonInfo, Xi/xiquerydevice.c:263-290).
        // Phase E, M20: GDK/Chromium classify axes and buttons by label.
        if (dev->has_buttons) {
          static const char* const kBtnLabels[10] = {
            "Button Left", "Button Middle", "Button Right",
            "Button Wheel Up", "Button Wheel Down",
            "Button Horiz Wheel Left", "Button Horiz Wheel Right",
            nullptr, nullptr, nullptr };
          const uint16_t num_buttons = 10;
          const uint16_t state_words = 1;   // bytes_to_int32(bits_to_bytes(10))
          const uint16_t length_words = (8 + state_words * 4 + num_buttons * 4) / 4;
          uint32_t state = 0;
          for (int b = 0; b < 10; b++)
            if (ctx.input().buttons & (1u << b)) state |= (1u << (b + 1));

          appendU16(1);              // type = ButtonClass (XI2: 1)
          appendU16(length_words);   // length in 4-byte words
          appendU16(dev->id);        // sourceid
          appendU16(num_buttons);    // num_buttons
          appendU32(state);          // button state mask (1-indexed bits)
          for (int b = 0; b < 10; b++) {
            const char* l = kBtnLabels[b];
            appendU32(l ? x11::AtomTable::instance().intern(l, std::strlen(l), false) : 0u);
          }

          // Two ValuatorClasses — xorg InitValuatorAxisStruct for the core
          // pointer (dix/devices.c:662-671, 1594-1607): labels Rel X / Rel Y,
          // Relative, min = max = NO_AXIS_LIMITS (−1), resolution 0; `value`
          // is the accumulated axis value, i.e. the current root position for
          // the master (ListValuatorInfo, Xi/xiquerydevice.c:358-380).  Device
          // events carry that same absolute position and raw events the
          // deltas — the split xorg makes for a relative core pointer (L4).
          //   type(2) length(2) sourceid(2) number(2) label(4) min(8) max(8)
          //   value(8) resolution(4) mode(1) pad(3) = 44 bytes = 11 words
          const int32_t axisVal[2] = { ctx.input().root_x_u, ctx.input().root_y_u };
          static const char* const kAxisLabels[2] = { "Rel X", "Rel Y" };
          for (uint16_t axis = 0; axis < 2; axis++) {
            appendU16(2);              // type = ValuatorClass
            appendU16(11);             // length = 11 words
            appendU16(dev->id);        // sourceid
            appendU16(axis);           // number
            appendU32(x11::AtomTable::instance().intern(kAxisLabels[axis],
                                                       std::strlen(kAxisLabels[axis]), false));
            appendU32(0xFFFFFFFFu); appendU32(0);            // min = -1 (FP3232)
            appendU32(0xFFFFFFFFu); appendU32(0);            // max = -1
            appendU32((uint32_t)axisVal[axis]); appendU32(0); // value
            appendU32(0);              // resolution
            payload.push_back(0);      // mode = Relative
            payload.push_back(0);      // pad
            payload.push_back(0);      // pad
            payload.push_back(0);      // pad
          }

          // NO ScrollClass.  xorg (xiquerydevice.c) only emits a ScrollInfo for
          // axes whose scroll.type != NONE, and those are SEPARATE axes from
          // x/y (e.g. 2,3), never the position valuators.  We previously
          // advertised ScrollClass on axes 0,1 — the same axes as x/y — so
          // Chromium read every pointer-motion valuator update as scroll input
          // ("contents move with the mouse").  We deliver scroll as core
          // buttons 4/5, so this device has no smooth-scroll valuators.
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
      reply[1] = 48;  // RepType = X_XIQueryDevice
      wire::wr16_le(reply.data() + 2, seq);
      wire::wr32_le(reply.data() + 4, payload_words);
      wire::wr16_le(reply.data() + 8, num_devices);
      std::memcpy(reply.data() + 32, payload.data(), payload.size());

      (void)ctx.reply().sendReplyRaw(reply.data(), reply.size());
      return;
    }

    // ---- minor 49: XISetFocus (void) ----
    // xorg Xi/xisetdevfocus.c:70-86: the device must have a focus class
    // (keyboards), then SetInputFocus(RevertToParent) — the same choreography
    // as core opcode 42 (Phase E, L7).
    case 49: {
      uint32_t focus = 0, time = 0; uint16_t dev = 0;
      if (br.remaining() >= 10) { focus = br.readU32(); time = br.readU32(); dev = br.readU16(); }
      br.skip(br.remaining());
      (void)time;
      if (!x11::xi2::isKnownDevice(dev) || !isKeyboardDev(dev)) { xiError(kBadDevice, dev); return; }
      const uint32_t newFocus = (focus == 0) ? 0u : (focus == 1) ? x11::kPointerRootFocus : focus;
      if (newFocus && newFocus != x11::kPointerRootFocus && !ctx.window(newFocus)) {
        xiError(x11::error::BadWindow, focus);
        return;
      }
      const uint32_t oldFocus = ctx.input().focus_xid;
      uint8_t mode = x11::notifymode::kNormal;
      {
        x11::KeyboardGrab kg{};
        if (ctx.grabs().getKeyboardGrabInfo(kg) && kg.active) mode = x11::notifymode::kWhileGrabbed;
      }
      if (auto* s = x11_proto_bridge_get_server())
        x11::focusev::doFocusEvents(ctx, s->eventOps(), oldFocus, newFocus, mode);
      ctx.input().focus_xid = newFocus;
      ctx.input().focus_revert_to = 2;   // RevertToParent
      return;
    }

    // ---- minor 50: XIGetFocus (reply-bearing) ----
    // Xi/xisetdevfocus.c:89-122: None / PointerRoot / the focus window.
    case 50: {
      uint16_t dev = 0;
      if (br.remaining() >= 2) dev = br.readU16();
      br.skip(br.remaining());
      if (!x11::xi2::isKnownDevice(dev) || !isKeyboardDev(dev)) { xiError(kBadDevice, dev); return; }
      const uint32_t f = ctx.input().focus_xid;
      const uint32_t focusWin = (f == 0) ? 0u : (f == x11::kPointerRootFocus) ? 1u : f;
      (void)ctx.reply().sendReply32(seq, [focusWin](std::array<uint8_t, 32>& rep) {
        rep[1] = 50;                              // RepType
        wire::wr32_le(rep.data() + 4, 0);       // length
        wire::wr32_le(rep.data() + 8, focusWin); // focus
      });
      return;
    }

    // ---- minor 51: XIGrabDevice (reply-bearing) ----
    // xorg Xi/xigrabdev.c ProcXIGrabDevice → dix/events.c GrabDevice: the
    // named DEVICE decides whether this is a pointer or a keyboard grab; the
    // request's XI2 mask is the grab's own mask (grab-time delivery is at the
    // XI2 level only — DeliverOneGrabbedEvent); status codes AlreadyGrabbed /
    // GrabNotViewable / GrabInvalidTime; activation sends the NotifyGrab
    // crossings (pointer) or the NotifyGrab focus pair (keyboard).
    //
    // GTK3's gdk_seat_grab issues one call for the master pointer (2) and one
    // for the master keyboard (3) and ungrabs them separately.  The previous
    // handler turned BOTH into a pointer grab, so the keyboard grab replaced
    // the pointer grab's record and the keyboard UNGRAB released the pointer
    // grab GTK still believed it held (M3 in docs/XI2_XORG_COMPARISON.md).
    case 51: {
      if (br.remaining() < 20) {
        br.skip(br.remaining());
        (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
          wire::wr32_le(rep.data() + 4, 0);
          rep[8] = 0; // status = Success (nothing to grab)
        });
        return;
      }
      const uint32_t win  = br.readU32();
      const uint32_t time = br.readU32();
      const uint32_t cursor = br.readU32();
      const uint16_t deviceid = br.readU16();
      const uint8_t grab_mode = br.readU8();
      const uint8_t paired    = br.readU8();
      const uint8_t owner_ev  = br.readU8();
      (void)br.readU8();                // pad
      const uint16_t mask_len = br.readU16();
      uint32_t xi2mask = 0;             // mask word 0 covers every type we emit (≤31)
      for (uint16_t j = 0; j < mask_len && br.remaining() >= 4; j++) {
        const uint32_t w = br.readU32();
        if (j == 0) xi2mask = w;
      }
      br.skip(br.remaining());

      const bool isKeyboard = (deviceid == x11::xi2::kVirtualCoreKeyboard ||
                               deviceid == x11::xi2::kXTESTKeyboard ||
                               deviceid == x11::xi2::kRealKeyboard);
      const bool isPointer  = (deviceid == x11::xi2::kVirtualCorePointer ||
                               deviceid == x11::xi2::kXTESTPointer ||
                               deviceid == x11::xi2::kRealPointer);
      if (!isKeyboard && !isPointer) {
        xiError(kBadDevice, deviceid);   // dixLookupDevice fails → BadDevice
        return;
      }
#ifndef NDEBUG
      if (grab_mode == 0 || paired == 0)   // GrabModeSync — accepted, never frozen (L23)
        fprintf(stderr, "[GRAB_SYNC] XIGrabDevice fd=%d device=%u win=0x%08X grab_mode=%u paired_mode=%u\n",
                ctx.transport().clientFd(), (unsigned)deviceid, (unsigned)win,
                (unsigned)grab_mode, (unsigned)paired);
#endif
      if (win != x11::kRootWindowXid) {
        x11::WindowView wv{};
        if (!ctx.windows().snapshot(win, wv)) {
          xiError(x11::error::BadWindow, win);
          return;
        }
      }

      const int      fd  = ctx.transport().clientFd();
      const uint32_t now = x11_now_ms_monotonic();
      auto* srv = x11_proto_bridge_get_server();
      uint8_t gs = x11::kGrabSuccess;

      if (isPointer) {
        x11::PointerGrab held{};
        const bool haveHeld = ctx.grabs().getPointerGrab(held) && held.active;
        if (haveHeld && (!held.is_xi2 || (held.owner_fd >= 0 && held.owner_fd != fd))) {
          gs = x11::kAlreadyGrabbed;
        } else if (!x11::grabchoreo::isViewable(ctx, win)) {
          gs = x11::kGrabNotViewable;
        } else if (!x11::grabTimeValid(time, now, haveHeld ? held.grab_time : 0)) {
          gs = x11::kGrabInvalidTime;
        } else {
          x11::PointerGrab req{};
          req.grabWindow    = win;
          req.ownerEvents   = (owner_ev != 0);
          // An XI2 grab delivers at the XI2 level only, filtered by xi2mask
          // (DeliverOneGrabbedEvent); the core mask plays no part.
          req.eventMask     = 0;
          req.owner_fd      = fd;
          req.grab_time     = time ? time : now;
          req.is_xi2        = true;
          req.xi2mask       = xi2mask;
          req.pointer_mode  = grab_mode;
          req.keyboard_mode = paired;
          req.cursor        = cursor;
          gs = ctx.grabs().tryPointerGrab(req);
          if (gs == x11::kGrabSuccess && srv) {
            // xorg ActivatePointerGrab (dix/events.c:1593-1611): Leave(old grab
            // window, else the sprite window) / Enter(grab window), NotifyGrab,
            // unless this window was already the grab window.  Chromium grabs
            // on every press; the old unconditional Enter(Grab) was spurious.
            const uint32_t from = haveHeld ? held.grabWindow : x11::grabchoreo::spriteWindow(ctx);
            if (!(haveHeld && held.grabWindow == win))
              x11::grabchoreo::pointerGrabCrossings(ctx, srv->eventOps(), from, win, /*NotifyGrab*/1);
            // xorg ActivatePointerGrab → PostNewCursor: the grab cursor shows
            // at once (L15).
            if (ctx.input().last_xid)
              x11::maybeApplyCursor(ctx, ctx.input().last_xid,
                                    ctx.input().routePointer(ctx.input().pointer_xid));
          }
        }
      } else {
        x11::KeyboardGrab held{};
        const bool haveHeld = ctx.grabs().getKeyboardGrabInfo(held) && held.active;
        if (haveHeld && (!held.is_xi2 || (held.owner_fd >= 0 && held.owner_fd != fd))) {
          gs = x11::kAlreadyGrabbed;
        } else if (!x11::grabchoreo::isViewable(ctx, win)) {
          gs = x11::kGrabNotViewable;
        } else if (!x11::grabTimeValid(time, now, haveHeld ? held.grab_time : 0)) {
          gs = x11::kGrabInvalidTime;
        } else {
          x11::KeyboardGrab req{};
          req.grabWindow  = win;
          req.ownerEvents = (owner_ev != 0);
          req.owner_fd    = fd;
          req.grab_time   = time ? time : now;
          req.is_xi2      = true;
          req.xi2mask     = xi2mask;
          gs = ctx.grabs().tryKeyboardGrab(req);
          if (gs == x11::kGrabSuccess && srv) {
            // xorg ActivateKeyboardGrab (dix/events.c:1720-1735): DoFocusEvents(
            // old grab window, else the focus window — None sends nothing —
            // → grab window, NotifyGrab), core + XI2, unless this window was
            // already the grab window (:1732-1734).
            const uint32_t from = haveHeld ? held.grabWindow : ctx.input().focus_xid;
            if (from && !(haveHeld && held.grabWindow == win))
              x11::grabchoreo::keyboardGrabFocusPair(ctx, srv->eventOps(), from, win, /*NotifyGrab*/1);
          }
        }
      }

      // XI2 GrabStatus codes match core (0..4).
      (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
        rep[1] = 51;                        // RepType
        wire::wr32_le(rep.data() + 4, 0);  // length = 0
        rep[8] = gs;                        // status at byte 8 (xXIGrabDeviceReply)
      });
      return;
    }

    // ---- minor 52: XIUngrabDevice (void) ----
    // xorg Xi/xigrabdev.c:148-172: the named device selects the grab; release
    // only an XI2 grab held by this client and only when the request time is
    // within [grab time, now].  Deactivation sends the NotifyUngrab crossings
    // (pointer: Leave grab window / Enter sprite window) or focus pair
    // (keyboard: FocusOut grab window / FocusIn focus window).
    case 52: {
      uint32_t time = 0; uint16_t deviceid = 0;
      if (br.remaining() >= 6) { time = br.readU32(); deviceid = br.readU16(); }
      br.skip(br.remaining());

      const bool isKeyboard = (deviceid == x11::xi2::kVirtualCoreKeyboard ||
                               deviceid == x11::xi2::kXTESTKeyboard ||
                               deviceid == x11::xi2::kRealKeyboard);
      const bool isPointer  = (deviceid == x11::xi2::kVirtualCorePointer ||
                               deviceid == x11::xi2::kXTESTPointer ||
                               deviceid == x11::xi2::kRealPointer);
      if (!isKeyboard && !isPointer) {
        xiError(kBadDevice, deviceid);
        return;
      }
      const int      fd  = ctx.transport().clientFd();
      const uint32_t now = x11_now_ms_monotonic();
      auto* srv = x11_proto_bridge_get_server();

      if (isPointer) {
        x11::PointerGrab held{};
        if (ctx.grabs().getPointerGrab(held) && held.active && held.is_xi2 &&
            held.owner_fd == fd && x11::ungrabTimeValid(time, now, held.grab_time)) {
          ctx.grabs().clearPointerGrab(fd);
          // xorg DeactivatePointerGrab (dix/events.c:1670, 1688-1689): grab
          // gone first, then Leave(grab window) / Enter(sprite window), and
          // PostNewCursor restores the window cursor (L15).
          if (srv)
            x11::grabchoreo::pointerGrabCrossings(ctx, srv->eventOps(), held.grabWindow,
                                                  x11::grabchoreo::spriteWindow(ctx), /*NotifyUngrab*/2);
          if (ctx.input().last_xid)
            x11::maybeApplyCursor(ctx, ctx.input().last_xid,
                                  ctx.input().routePointer(ctx.input().pointer_xid));
        }
      } else {
        x11::KeyboardGrab held{};
        if (ctx.grabs().getKeyboardGrabInfo(held) && held.active && held.is_xi2 &&
            held.owner_fd == fd && x11::ungrabTimeValid(time, now, held.grab_time)) {
          ctx.grabs().clearKeyboardGrab(fd);
          // xorg DeactivateKeyboardGrab (dix/events.c:1772-1782): grab window
          // → focus window (None / PointerRoot: FocusOut side only).
          if (srv) {
            x11::grabchoreo::keyboardGrabFocusPair(ctx, srv->eventOps(), held.grabWindow,
                                                   ctx.input().focus_xid, /*NotifyUngrab*/2);
          }
        }
      }
      return;
    }

    // ---- minor 53: XIAllowEvents (void) ----
    // Sync grab modes never freeze here (every target client grabs async),
    // so AllowEvents has nothing to release; log the mode so a sync user
    // shows up in the trace (L23).  xXIAllowEventsReq: time(4) deviceid(2)
    // mode(1) pad(1) [touchid(4) grab_window(4) in 2.2].
    case 53: {
      uint16_t dev = 0; uint8_t mode = 0;
      if (br.remaining() >= 8) { (void)br.readU32(); dev = br.readU16(); mode = br.readU8(); }
      br.skip(br.remaining());
#ifndef NDEBUG
      fprintf(stderr, "[GRAB_SYNC] XIAllowEvents fd=%d device=%u mode=%u (no frozen device to thaw)\n",
              ctx.transport().clientFd(), (unsigned)dev, (unsigned)mode);
#endif
      return;
    }
    case 55: // XIPassiveUngrabDevice (void)
      br.skip(br.remaining());
      return;

    // ---- minor 54: XIPassiveGrabDevice (reply-bearing) ----
    // xorg Xi/xipassivegrab.c:79-258 (Phase E, M21): validate device, grab
    // type, detail and window, then reply with the modifier combinations
    // that FAILED — none here.  The grab itself is not recorded (no target
    // client installs XI2 passive grabs; the core GrabButton table stays the
    // one passive-grab store), so every combination reports success rather
    // than the BadRequest that made Xlib's default handler exit.
    case 54: {
      if (br.remaining() < 28) { xiError(x11::error::BadLength, 0); return; }
      (void)br.readU32();                       // time
      const uint32_t grab_window = br.readU32();
      (void)br.readU32();                       // cursor
      const uint32_t detail      = br.readU32();
      const uint16_t deviceid    = br.readU16();
      const uint16_t num_mods    = br.readU16();
      const uint16_t mask_len    = br.readU16();
      const uint8_t  grab_type   = br.readU8();
      (void)br.readU8();                        // grab_mode
      (void)br.readU8();                        // paired_device_mode
      (void)br.readU8();                        // owner_events
      br.skip(br.remaining());                  // pad, mask words, modifiers
      (void)num_mods; (void)mask_len;
      if (deviceid != x11::xi2::kAllDevices && deviceid != x11::xi2::kAllMasterDevices &&
          !x11::xi2::isKnownDevice(deviceid)) { xiError(kBadDevice, deviceid); return; }
      if (grab_type > 6) { xiError(x11::error::BadValue, grab_type); return; }           // :113-122
      if (grab_type >= 2 && detail != 0) { xiError(x11::error::BadValue, detail); return; } // :124-131
      if (grab_window != x11::kRootWindowXid && !ctx.window(grab_window)) {
        xiError(x11::error::BadWindow, grab_window);
        return;
      }
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 54;                          // RepType
        wire::wr32_le(rep.data() + 4, 0);   // length: no xXIGrabModifierInfo follow
        wire::wr16_le(rep.data() + 8, 0);   // num_modifiers (failed) = 0
      });
      return;
    }

    // ---- minor 56: XIListProperties (reply-bearing) ----
    // Xi/xiproperty.c:1103-1105: the device must exist (BadDevice); we
    // advertise no device properties (Phase E, L21).
    case 56: {
      uint16_t dev = 0;
      if (br.remaining() >= 2) dev = br.readU16();
      br.skip(br.remaining());
      if (!x11::xi2::isKnownDevice(dev)) { xiError(kBadDevice, dev); return; }
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 56;                          // RepType
        wire::wr32_le(rep.data() + 4, 0);   // length
        wire::wr16_le(rep.data() + 8, 0);   // num_properties = 0
      });
      return;
    }

    // ---- minor 57-58: XIChangeProperty / XIDeleteProperty (void) ----
    case 57:
    case 58:
      br.skip(br.remaining());
      return;

    // ---- minor 59: XIGetProperty (reply-bearing) ----
    // Xi/xiproperty.c:1198-1201: device must exist; a property we do not
    // hold reads back as type None, format 0, no items.
    case 59: {
      uint16_t dev = 0;
      if (br.remaining() >= 2) dev = br.readU16();
      br.skip(br.remaining());
      if (!x11::xi2::isKnownDevice(dev)) { xiError(kBadDevice, dev); return; }
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 59;                          // RepType
        wire::wr32_le(rep.data() + 4, 0);   // length
        wire::wr32_le(rep.data() + 8, 0);   // type = None
        wire::wr32_le(rep.data() + 12, 0);  // bytes_after
        wire::wr32_le(rep.data() + 16, 0);  // num_items
        rep[20] = 0;                          // format
      });
      return;
    }

    // ---- minor 60: XIGetSelectedEvents (reply-bearing) ----
    case 60: {
      // XIGetSelectedEvents: CARD32 window
      // Phase C (M4): the CALLER's entries on the window, one xXIEventMask per
      // device spec in ascending id order (Xi/xiselectev.c:342-428).
      uint32_t window = (br.remaining() >= 4) ? br.readU32() : 0;
      br.skip(br.remaining());
      const int fd = ctx.transport().clientFd();
      std::vector<std::pair<uint16_t, uint32_t>> list;
      if (!ctx.window(window)) {   // R1 Phase 3: root reads back from WindowTable like any window
        (void)ctx.transport().sendErrorExt(x11::error::BadWindow, seq, window, 60, ext::kXInput2);
        return;
      } else {
        for (const auto& m : ctx.windows().xi2MasksFor(window, fd)) list.push_back({m.deviceid, m.mask});
      }
      std::sort(list.begin(), list.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      std::vector<uint8_t> rep(32 + list.size() * 8, 0);
      rep[0] = 1;
      rep[1] = 60;                                            // RepType = minor
      wire::wr16_le(rep.data() + 2, seq);
      wire::wr32_le(rep.data() + 4, (uint32_t)(list.size() * 2));   // 2 words per mask
      wire::wr16_le(rep.data() + 8, (uint16_t)list.size());   // num_masks
      size_t off = 32;
      for (const auto& e : list) {
        wire::wr16_le(rep.data() + off + 0, e.first);   // deviceid
        wire::wr16_le(rep.data() + off + 2, 1);         // mask_len = 1 word
        wire::wr32_le(rep.data() + off + 4, e.second);  // mask
        off += 8;
      }
      (void)ctx.reply().sendReplyRaw(rep.data(), rep.size());
      return;
    }

    // ---- minor 61: XIBarrierReleasePointer (void) ----
    // Xi/xibarriers.c:862-912 — no barriers exist here (CreatePointerBarrier
    // is a silent XFIXES stub), so there is nothing to release (L22).
    case 61:
      br.skip(br.remaining());
      return;

    default:
      break;
    }
    // Unhandled XI2 minor — send BadRequest so XCB sequence stays in sync.
    // Without an error reply, a reply-bearing minor we haven't implemented
    // would leave XCB hanging, eventually causing a sequence desync crash.
    { char buf[128]; snprintf(buf, sizeof(buf), "[XInput2] unhandled minor=%u seq=%u — sending BadRequest\n", (unsigned)minor, (unsigned)seq); x11_ui_push_log(1, buf); }
    xiError(x11::error::BadRequest, 0);
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
      // v2.0, NOT 2.2: reporting 2.2 lets clients call GrabControl, which
      // triggers the unresolved Vivado segfault (see CLAUDE.md "XTEST /
      // GrabControl Crash Investigation").  The v1.19.14 downgrade was
      // found missing from the code in the 2026-08-31 adversarial review
      // (docs claimed 2.0, code shipped 2.2) — restored in v1.19.36.8.
      wire::wr16_le(rep.data() + 8, 0);     // server_minor_version (2.0)
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
      // Body (from br, wire offset 4): type(1), detail(1), pad(2), time(4),
      // root(4), pad(4), pad(4), rootX(2), rootY(2).  xorg feeds the synthesized
      // event into the normal input pipeline; we do the same via the host-command
      // bridge, so grabs and routing see it exactly like real input.  `time`
      // (delay) is honoured as immediate and `root` as the single screen.
      // Types: KeyPress 2, KeyRelease 3, ButtonPress 4, ButtonRelease 5,
      // MotionNotify 6 (detail 0 = absolute rootX/rootY, 1 = relative delta).
      if (br.remaining() < 24) { br.skip(br.remaining()); return; }
      const uint8_t  type   = br.readU8();
      const uint8_t  detail = br.readU8();
      (void)br.readU16();                        // pad
      (void)br.readU32();                        // time (delay) — immediate
      (void)br.readU32();                        // root — single screen
      (void)br.readU32(); (void)br.readU32();    // pad, pad
      const int16_t  rootX  = br.readI16();
      const int16_t  rootY  = br.readI16();
      br.skip(br.remaining());

      const uint32_t buttons = ctx.input().buttons;
      const uint32_t mods    = ctx.input().mods;

      switch (type) {
        case 2:   // KeyPress
        case 3: { // KeyRelease
          // `detail` is an X11 keycode; the Key host-cmd handler adds +8
          // (mac VK → X11), so pass detail-8.  Route to the keyboard focus:
          // the handler needs a non-zero host for its guard, so give it the
          // focus window's toplevel (falling back to the pointer host).
          if (detail < 8) break;
          const uint32_t fwin = ctx.input().focus_xid;
          uint32_t fhost = (fwin && fwin != x11::kPointerRootFocus)
                             ? ctx.windows().topLevelAncestorOf(fwin) : 0;
          if (!fhost) fhost = (fwin && fwin != x11::kPointerRootFocus) ? fwin : ctx.input().last_xid;
          x11_post_key_event(fhost, type == 2, (uint32_t)(detail - 8), mods,
                             /*is_repeat=*/false, /*utf8_text=*/nullptr);
          break;
        }
        case 4:   // ButtonPress
        case 5: { // ButtonRelease
          // No position in the request — xorg uses the current pointer.  Deliver
          // at the pointer's host so passive/active grabs and normal routing all
          // see the real sprite (this is what verifies C1).
          uint32_t host = ctx.input().last_xid;
          if (!host) host = ctx.input().focus_host;
          if (!host) break;
          x11_post_pointer_button(host, type == 4, detail,
                                  ctx.input().win_x_u, ctx.input().win_y_u,
                                  ctx.input().root_x_u, ctx.input().root_y_u,
                                  buttons, mods);
          break;
        }
        case 6: { // MotionNotify — detail 0 absolute, 1 relative
          const int32_t nrx = (detail == 1) ? ctx.input().root_x_u + rootX : rootX;
          const int32_t nry = (detail == 1) ? ctx.input().root_y_u + rootY : rootY;
          // Toplevel host containing the destination (rootless: root's children).
          uint32_t host = 0; int32_t lx = nrx, ly = nry;
          for (uint32_t top : ctx.windows().childrenInStackOrder(x11::kRootWindowXid)) {
            x11::WindowView vw{};
            if (!ctx.windows().snapshot(top, vw) || !vw.mapped) continue;
            const int32_t bw = (int32_t)vw.border_width;
            if (nrx >= (int32_t)vw.x - bw && nrx < (int32_t)vw.x + (int32_t)vw.w + bw &&
                nry >= (int32_t)vw.y - bw && nry < (int32_t)vw.y + (int32_t)vw.h + bw) {
              host = top; lx = nrx - (int32_t)vw.x; ly = nry - (int32_t)vw.y;
            }
          }
          // deliver=1 over a window (postMotion routes + crossings), else a
          // window-free position update keeps QueryPointer correct.
          x11_post_pointer_move2(host, lx, ly, nrx, nry, host ? 1 : 0, buttons, mods);
          break;
        }
        default:
          break;   // unknown fake type — ignore
      }
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
    //   Benign no-ops in a rootless server: there is no root compositor and
    //   nothing to redirect (each top-level window is its own NSWindow surface).
    case 1: case 2: case 3: case 4: case 5:
      br.skip(br.remaining());
      return;

    // ---- minor 6: CompositeNameWindowPixmap ----
    //   M4/honesty: we never redirect, so there is no named window pixmap to
    //   hand out.  The reference server (composite/compext.c) returns BadMatch
    //   when the window has no CompWindow (i.e. is not redirected); do the same
    //   instead of silently "succeeding" and leaking a phantom pixmap XID that
    //   would fault the moment the client used it (GetImage/CopyArea/CreatePicture).
    case 6:
      br.skip(br.remaining());
      ctx.transport().sendErrorCore(x11::error::BadMatch, seq, 0, major);
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
