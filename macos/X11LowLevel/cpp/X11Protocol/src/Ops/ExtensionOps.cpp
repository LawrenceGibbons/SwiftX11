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
#include "Ops/ReplyWriter.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Utils/ByteReader.hpp"
#include "Utils/WireLE.hpp"
#include "Core/X11ExtOpcodes.hpp"

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
    case 1: // ShapeRectangles — consume silently (all windows remain rectangular)
    case 2: // ShapeMask — consume silently
    case 3: // ShapeCombine — consume silently
    case 4: // ShapeOffset — consume silently
      br.skip(br.remaining());
      return;
    case 5: {
      // ShapeQueryExtents — reply with window bounding rect
      if (br.remaining() < 4) { br.skip(br.remaining()); return; }
      const uint32_t wid = br.readU32();
      br.skip(br.remaining());

      // Look up window geometry
      uint16_t ww = 0, wh = 0;
      WindowView vw{};
      if (ctx.windows().snapshot(wid, vw)) {
        ww = vw.w;
        wh = vw.h;
      }

      (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0); // length
        rep[8]  = 0; // boundingShaped = false
        rep[9]  = 0; // clipShaped = false
        // Bounding extents: x=0, y=0, w, h
        wire::wr16_le(rep.data() + 12, 0);  // bounding x
        wire::wr16_le(rep.data() + 14, 0);  // bounding y
        wire::wr16_le(rep.data() + 16, ww); // bounding width
        wire::wr16_le(rep.data() + 18, wh); // bounding height
        // Clip extents: same as bounding
        wire::wr16_le(rep.data() + 20, 0);  // clip x
        wire::wr16_le(rep.data() + 22, 0);  // clip y
        wire::wr16_le(rep.data() + 24, ww); // clip width
        wire::wr16_le(rep.data() + 26, wh); // clip height
      });
      return;
    }
    case 6: // ShapeSelectInput — consume silently
      br.skip(br.remaining());
      return;
    case 7: {
      // ShapeInputSelected — reply with enabled=false
      br.skip(br.remaining());
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0); // length
        rep[1] = 0; // enabled = false
      });
      return;
    }
    case 8: {
      // ShapeGetRectangles — reply with single bounding rect
      if (br.remaining() < 8) { br.skip(br.remaining()); return; }
      const uint32_t wid = br.readU32();
      /*kind*/ br.readU8();
      br.skip(br.remaining());

      uint16_t ww = 0, wh = 0;
      WindowView vw{};
      if (ctx.windows().snapshot(wid, vw)) {
        ww = vw.w;
        wh = vw.h;
      }

      // Reply: 1 rectangle (8 bytes)
      uint8_t rectPayload[8] = {};
      wire::wr16_le(rectPayload + 0, 0);  // x
      wire::wr16_le(rectPayload + 2, 0);  // y
      wire::wr16_le(rectPayload + 4, ww); // width
      wire::wr16_le(rectPayload + 6, wh); // height

      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        rep[1] = 0; // ordering = UnSorted
        wire::wr32_le(rep.data() + 4, 2); // length = 8 bytes / 4 = 2 words
        wire::wr32_le(rep.data() + 8, 1); // nrects = 1
      });
      ctx.reply().sendBytes(rectPayload, sizeof(rectPayload));
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
