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
#include "Ops/ReplyWriter.hpp"
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
  // XFIXES — minor 0 = QueryVersion
  // -------------------------------------------------------------------
  if (major == ext::kXFIXES) {
    if (minor == 0) {
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
    // Other XFIXES sub-opcodes: silently consume
#ifndef NDEBUG
    fprintf(stderr, "[XFIXES] unhandled minor=%u seq=%u\n", (unsigned)minor, (unsigned)seq);
#endif
    br.skip(br.remaining());
    return;
  }

  // -------------------------------------------------------------------
  // SHAPE — minor 0 = QueryVersion
  // -------------------------------------------------------------------
  if (major == ext::kSHAPE) {
    if (minor == 0) {
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
#ifndef NDEBUG
    fprintf(stderr, "[SHAPE] unhandled minor=%u seq=%u\n", (unsigned)minor, (unsigned)seq);
#endif
    br.skip(br.remaining());
    return;
  }

  // -------------------------------------------------------------------
  // RANDR — minor 0 = RRQueryVersion, minor 5 = RRGetScreenInfo
  //         minor 8 = RRGetScreenResources, etc.
  // -------------------------------------------------------------------
  if (major == ext::kRANDR) {
    if (minor == 0) {
      // RRQueryVersion: CARD32 client_major, CARD32 client_minor
      br.skip(br.remaining());
      // Reply: major=1, minor=5
      (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
        wire::wr32_le(rep.data() + 4, 0);  // length
        wire::wr32_le(rep.data() + 8, 1);  // server major
        wire::wr32_le(rep.data() + 12, 5); // server minor
      });
      return;
    }
#ifndef NDEBUG
    fprintf(stderr, "[RANDR] unhandled minor=%u seq=%u\n", (unsigned)minor, (unsigned)seq);
#endif
    br.skip(br.remaining());
    return;
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
#ifndef NDEBUG
    fprintf(stderr, "[Xinerama] unhandled minor=%u seq=%u\n", (unsigned)minor, (unsigned)seq);
#endif
    br.skip(br.remaining());
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
#ifndef NDEBUG
    fprintf(stderr, "[GE] unhandled minor=%u seq=%u\n", (unsigned)minor, (unsigned)seq);
#endif
    br.skip(br.remaining());
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
