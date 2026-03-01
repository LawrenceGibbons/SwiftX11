//
//  RenderOps.cpp
//  X11LowLevel
//
//  RENDER extension — minimal but functional implementation.
//
//  Phase 1: QueryVersion, QueryPictFormats, CreatePicture, ChangePicture,
//           FreePicture, Composite (PictOpSrc/Over), FillRectangles,
//           CreateSolidFill, QueryFilters, stubs for glyph ops and transforms.
//

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <mutex>

#include "Ops/RenderOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/ByteReader.hpp"
#include "Utils/WireLE.hpp"
#include "Core/X11ExtOpcodes.hpp"
#include "Core/DrawableRW.hpp"
#include "Core/WindowTable.hpp"
#include "Damage.hpp"

namespace x11 {

// ============================================================================
// Constants
// ============================================================================
static constexpr uint32_t kRootVisualId = 0x00000021u;

// PictFormat IDs (server-assigned)
static constexpr uint32_t kFmtARGB32 = 0x00000024u;
static constexpr uint32_t kFmtRGB24  = 0x00000025u;
static constexpr uint32_t kFmtA8     = 0x00000026u;
static constexpr uint32_t kFmtA4     = 0x00000027u;
static constexpr uint32_t kFmtA1     = 0x00000028u;

// PictOp (compositing operators)
enum PictOp : uint8_t {
  PictOpClear    = 0,
  PictOpSrc      = 1,
  PictOpDst      = 2,
  PictOpOver     = 3,
  PictOpOverReverse = 4,
  PictOpIn       = 5,
  PictOpInReverse = 6,
  PictOpOut      = 7,
  PictOpOutReverse = 8,
  PictOpAtop     = 9,
  PictOpAtopReverse = 10,
  PictOpXor      = 11,
  PictOpAdd      = 12,
};

// ============================================================================
// Picture table — maps Picture XID → state
// ============================================================================
struct PictureState {
  uint32_t drawable = 0;
  uint32_t format   = 0;
  bool     repeat   = false;
  bool     isSolid  = false;
  // Solid fill color (premultiplied ARGB8888)
  uint32_t solidARGB = 0;
  // Clip (not implemented — just tracked)
  bool     hasClip = false;
};

static std::mutex sPicMtx;
static std::unordered_map<uint32_t, PictureState> sPictures;

static PictureState* findPicture(uint32_t pid) {
  auto it = sPictures.find(pid);
  return (it != sPictures.end()) ? &it->second : nullptr;
}

// ============================================================================
// GlyphSet table — minimal stub (just track existence)
// ============================================================================
static std::mutex sGlyphMtx;
static std::unordered_map<uint32_t, bool> sGlyphSets; // XID → exists

// ============================================================================
// Inline compositing helpers
// ============================================================================

// Premultiply: convert straight-alpha ARGB to premultiplied
static inline uint32_t premultiply(uint32_t c) {
  const uint32_t a = (c >> 24) & 0xFF;
  if (a == 255) return c;
  if (a == 0) return 0;
  uint32_t r = ((c >> 16) & 0xFF) * a / 255;
  uint32_t g = ((c >> 8)  & 0xFF) * a / 255;
  uint32_t b = ((c >> 0)  & 0xFF) * a / 255;
  return (a << 24) | (r << 16) | (g << 8) | b;
}

// Over: src over dst (both premultiplied ARGB)
static inline uint32_t compositeOver(uint32_t dst, uint32_t src) {
  const uint32_t sa = (src >> 24) & 0xFF;
  if (sa == 255) return src;
  if (sa == 0) return dst;

  const uint32_t ia = 255 - sa; // inverse alpha
  auto blend = [ia](uint32_t s, uint32_t d) -> uint32_t {
    return s + (d * ia + 127) / 255;
  };

  const uint32_t a = blend((src >> 24) & 0xFF, (dst >> 24) & 0xFF);
  const uint32_t r = blend((src >> 16) & 0xFF, (dst >> 16) & 0xFF);
  const uint32_t g = blend((src >>  8) & 0xFF, (dst >>  8) & 0xFF);
  const uint32_t b = blend((src >>  0) & 0xFF, (dst >>  0) & 0xFF);

  return (std::min(a, 255u) << 24) | (std::min(r, 255u) << 16)
       | (std::min(g, 255u) << 8)  |  std::min(b, 255u);
}

// Apply compositing op
static inline uint32_t applyOp(uint8_t op, uint32_t dst, uint32_t src) {
  switch (op) {
    case PictOpClear: return 0xFF000000u; // opaque black
    case PictOpSrc:   return src | 0xFF000000u;
    case PictOpDst:   return dst;
    case PictOpOver:  return compositeOver(dst, src) | 0xFF000000u;
    case PictOpAdd: {
      auto add = [](uint32_t a, uint32_t b) -> uint32_t { return std::min(a + b, 255u); };
      return (add((dst >> 24) & 0xFF, (src >> 24) & 0xFF) << 24) |
             (add((dst >> 16) & 0xFF, (src >> 16) & 0xFF) << 16) |
             (add((dst >>  8) & 0xFF, (src >>  8) & 0xFF) <<  8) |
              add((dst >>  0) & 0xFF, (src >>  0) & 0xFF);
    }
    default:
      // Fallback: PictOpOver for unsupported ops
      return compositeOver(dst, src) | 0xFF000000u;
  }
}

// Convert 16-bit premultiplied RENDER color to ARGB8888
static inline uint32_t renderColorToARGB(uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
  return ((uint32_t)(a >> 8) << 24) | ((uint32_t)(r >> 8) << 16)
       | ((uint32_t)(g >> 8) << 8)  |  (uint32_t)(b >> 8);
}


// ============================================================================
// Registration
// ============================================================================
RenderOps::RenderOps(XProtoRegistrar& reg) {
  reg.registerMajor(ext::kRENDER, &RenderOps::onMajor, this);
}

void RenderOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<RenderOps*>(user)->handle(ctx, dc);
}


// ============================================================================
// Dispatch
// ============================================================================
void RenderOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  const uint8_t minor = dc.minor;
  const uint16_t seq  = dc.seq;
  ByteReader& br      = dc.br;

  switch (minor) {

  // ---- 0: QueryVersion ----
  case 0: {
    br.skip(br.remaining()); // consume client major/minor
    (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
      wire::wr32_le(rep.data() + 4, 0);  // length
      wire::wr32_le(rep.data() + 8, 0);  // major version
      wire::wr32_le(rep.data() + 12, 11); // minor version (0.11)
    });
    return;
  }

  // ---- 1: QueryPictFormats ----
  case 1: {
    br.skip(br.remaining());

    // Build the reply payload
    //
    // 5 formats × 28 bytes = 140 bytes
    // 1 screen:
    //   xPictScreen (8 bytes): nDepth=1, fallback=kFmtRGB24
    //   xPictDepth  (8 bytes): depth=24, nPictVisuals=1
    //   xPictVisual (8 bytes): visual=kRootVisualId, format=kFmtRGB24
    // 1 subpixel (4 bytes)
    //
    // Total payload: 140 + 8 + 8 + 8 + 4 = 168 bytes = 42 words

    static constexpr uint32_t kNumFormats  = 5;
    static constexpr uint32_t kNumScreens  = 1;
    static constexpr uint32_t kNumDepths   = 1;
    static constexpr uint32_t kNumVisuals  = 1;
    static constexpr uint32_t kNumSubpixel = 1;

    std::vector<uint8_t> payload(168, 0);
    uint8_t* p = payload.data();

    // --- Format 0: ARGB32 (28 bytes) ---
    auto writeFormat = [&](int off, uint32_t id, uint8_t depth,
                           uint16_t rShift, uint16_t rMask,
                           uint16_t gShift, uint16_t gMask,
                           uint16_t bShift, uint16_t bMask,
                           uint16_t aShift, uint16_t aMask) {
      wire::wr32_le(p + off + 0, id);
      p[off + 4] = 1; // type = Direct
      p[off + 5] = depth;
      wire::wr16_le(p + off + 8,  rShift);
      wire::wr16_le(p + off + 10, rMask);
      wire::wr16_le(p + off + 12, gShift);
      wire::wr16_le(p + off + 14, gMask);
      wire::wr16_le(p + off + 16, bShift);
      wire::wr16_le(p + off + 18, bMask);
      wire::wr16_le(p + off + 20, aShift);
      wire::wr16_le(p + off + 22, aMask);
      wire::wr32_le(p + off + 24, 0); // colormap = None
    };

    writeFormat(  0, kFmtARGB32, 32, 16, 0xFF,  8, 0xFF,  0, 0xFF, 24, 0xFF);
    writeFormat( 28, kFmtRGB24,  24, 16, 0xFF,  8, 0xFF,  0, 0xFF,  0, 0x00);
    writeFormat( 56, kFmtA8,      8,  0, 0x00,  0, 0x00,  0, 0x00,  0, 0xFF);
    writeFormat( 84, kFmtA4,      4,  0, 0x00,  0, 0x00,  0, 0x00,  0, 0x0F);
    writeFormat(112, kFmtA1,      1,  0, 0x00,  0, 0x00,  0, 0x00,  0, 0x01);

    // --- Screen 0 ---
    const int sOff = 140;
    // xPictScreen: nDepth=1, fallback=kFmtRGB24
    wire::wr32_le(p + sOff + 0, 1);          // nDepth
    wire::wr32_le(p + sOff + 4, kFmtRGB24);  // fallback

    // xPictDepth: depth=24, nPictVisuals=1
    p[sOff + 8] = 24; // depth
    wire::wr16_le(p + sOff + 10, 1); // nPictVisuals

    // xPictVisual: visual=kRootVisualId, format=kFmtRGB24
    wire::wr32_le(p + sOff + 16, kRootVisualId);
    wire::wr32_le(p + sOff + 20, kFmtRGB24);

    // Subpixel order: SubPixelNone = 5
    wire::wr32_le(p + sOff + 24, 5);

    const uint32_t lenWords = (uint32_t)(payload.size() / 4u);

    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      wire::wr32_le(rep.data() +  4, lenWords);
      wire::wr32_le(rep.data() +  8, kNumFormats);
      wire::wr32_le(rep.data() + 12, kNumScreens);
      wire::wr32_le(rep.data() + 16, kNumDepths);
      wire::wr32_le(rep.data() + 20, kNumVisuals);
      wire::wr32_le(rep.data() + 24, kNumSubpixel);
    });
    ctx.reply().sendBytes(payload.data(), payload.size());
    return;
  }

  // ---- 2: QueryPictIndexValues ----
  case 2: {
    br.skip(br.remaining());
    // Empty reply (no indexed formats)
    (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
      wire::wr32_le(rep.data() + 4, 0); // length
      wire::wr32_le(rep.data() + 8, 0); // numValues
    });
    return;
  }

  // ---- 4: CreatePicture ----
  case 4: {
    if (br.remaining() < 16) { br.skip(br.remaining()); return; }
    const uint32_t pid      = br.readU32();
    const uint32_t drawable = br.readU32();
    const uint32_t format   = br.readU32();
    const uint32_t mask     = br.readU32();

    PictureState ps;
    ps.drawable = drawable;
    ps.format   = format;

    // Parse value list
    if (mask & (1u << 0)) { // CPRepeat
      if (br.remaining() >= 4) ps.repeat = (br.readU32() != 0);
    }
    // Consume remaining value list items
    br.skip(br.remaining());

    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      sPictures[pid] = ps;
    }
    return;
  }

  // ---- 5: ChangePicture ----
  case 5: {
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }
    const uint32_t pid  = br.readU32();
    const uint32_t mask = br.readU32();

    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* ps = findPicture(pid);
      if (ps && (mask & (1u << 0)) && br.remaining() >= 4) {
        ps->repeat = (br.readU32() != 0);
      }
    }
    br.skip(br.remaining());
    return;
  }

  // ---- 6: SetPictureClipRectangles ----
  case 6: {
    br.skip(br.remaining()); // consume silently
    return;
  }

  // ---- 7: FreePicture ----
  case 7: {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t pid = br.readU32();
    br.skip(br.remaining());

    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      sPictures.erase(pid);
    }
    return;
  }

  // ---- 8: Composite ----
  case 8: {
    if (br.remaining() < 32) { br.skip(br.remaining()); return; }
    const uint8_t  op     = br.readU8();
    br.skip(3); // pad
    const uint32_t srcPid = br.readU32();
    const uint32_t mskPid = br.readU32();
    const uint32_t dstPid = br.readU32();
    const int16_t  xSrc   = (int16_t)br.readU16();
    const int16_t  ySrc   = (int16_t)br.readU16();
    const int16_t  xMask  = (int16_t)br.readU16();
    const int16_t  yMask  = (int16_t)br.readU16();
    const int16_t  xDst   = (int16_t)br.readU16();
    const int16_t  yDst   = (int16_t)br.readU16();
    const uint16_t width  = br.readU16();
    const uint16_t height = br.readU16();
    br.skip(br.remaining());
    (void)xMask; (void)yMask; (void)mskPid;

    // Resolve source
    uint32_t srcColor = 0xFF000000u; // default: opaque black
    bool srcIsSolid = false;
    uint32_t srcDrawable = 0;
    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* sps = findPicture(srcPid);
      if (sps) {
        srcIsSolid   = sps->isSolid;
        srcColor     = sps->solidARGB;
        srcDrawable  = sps->drawable;
      }
    }

    // Resolve destination
    uint32_t dstDrawable = 0;
    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* dps = findPicture(dstPid);
      if (dps) dstDrawable = dps->drawable;
    }
    if (dstDrawable == 0) return;

    DrawableRW dst{};
    if (!resolveDrawableRW(ctx, dstDrawable, dst)) return;
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0) return;

    if (srcIsSolid) {
      // Solid fill composite
      for (int32_t row = 0; row < (int32_t)height; row++) {
        const int32_t dy = (int32_t)yDst + row;
        if (dy < 0 || dy >= (int32_t)dst.h) continue;
        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        for (int32_t col = 0; col < (int32_t)width; col++) {
          const int32_t dx = (int32_t)xDst + col;
          if (dx < 0 || dx >= (int32_t)dst.w) continue;
          drow[(size_t)dx] = applyOp(op, drow[(size_t)dx], srcColor);
        }
      }
    } else if (srcDrawable != 0) {
      // Drawable-to-drawable composite
      DrawableRW src{};
      if (!resolveDrawableRW(ctx, srcDrawable, src)) return;
      if (!src.pixels32) return;

      for (int32_t row = 0; row < (int32_t)height; row++) {
        const int32_t sy = (int32_t)ySrc + row;
        const int32_t dy = (int32_t)yDst + row;
        if (dy < 0 || dy >= (int32_t)dst.h) continue;
        if (sy < 0 || sy >= (int32_t)src.h) continue;

        const uint32_t* srow = src.pixels32 + (size_t)sy * (size_t)src.stridePixels;
        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;

        for (int32_t col = 0; col < (int32_t)width; col++) {
          const int32_t sx = (int32_t)xSrc + col;
          const int32_t dx = (int32_t)xDst + col;
          if (dx < 0 || dx >= (int32_t)dst.w) continue;
          if (sx < 0 || sx >= (int32_t)src.w) continue;
          drow[(size_t)dx] = applyOp(op, drow[(size_t)dx], srow[(size_t)sx]);
        }
      }
    }

    if (dst.isWindow) {
      damageOrDirty(ctx, dstDrawable, xDst, yDst, (int32_t)width, (int32_t)height);
    }
    return;
  }

  // ---- 17: CreateGlyphSet ----
  case 17: {
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }
    const uint32_t gsid   = br.readU32();
    /*format*/ br.readU32();
    br.skip(br.remaining());
    {
      std::lock_guard<std::mutex> lk(sGlyphMtx);
      sGlyphSets[gsid] = true;
    }
    return;
  }

  // ---- 18: ReferenceGlyphSet ----
  case 18: {
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }
    const uint32_t gsid = br.readU32();
    /*existing*/ br.readU32();
    br.skip(br.remaining());
    {
      std::lock_guard<std::mutex> lk(sGlyphMtx);
      sGlyphSets[gsid] = true;
    }
    return;
  }

  // ---- 19: FreeGlyphSet ----
  case 19: {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t gsid = br.readU32();
    br.skip(br.remaining());
    {
      std::lock_guard<std::mutex> lk(sGlyphMtx);
      sGlyphSets.erase(gsid);
    }
    return;
  }

  // ---- 20: AddGlyphs ----
  // ---- 22: FreeGlyphs ----
  // ---- 23: CompositeGlyphs8 ----
  // ---- 24: CompositeGlyphs16 ----
  // ---- 25: CompositeGlyphs32 ----
  case 20: case 22: case 23: case 24: case 25: {
    // Stub: consume silently. No glyph rendering yet.
    br.skip(br.remaining());
    return;
  }

  // ---- 26: FillRectangles ----
  case 26: {
    if (br.remaining() < 16) { br.skip(br.remaining()); return; }
    const uint8_t op = br.readU8();
    br.skip(3); // pad
    const uint32_t dstPid = br.readU32();
    const uint16_t cR = br.readU16();
    const uint16_t cG = br.readU16();
    const uint16_t cB = br.readU16();
    const uint16_t cA = br.readU16();

    const uint32_t fillColor = renderColorToARGB(cR, cG, cB, cA);

    // Resolve destination
    uint32_t dstDrawable = 0;
    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* dps = findPicture(dstPid);
      if (dps) dstDrawable = dps->drawable;
    }
    if (dstDrawable == 0) { br.skip(br.remaining()); return; }

    DrawableRW dst{};
    if (!resolveDrawableRW(ctx, dstDrawable, dst)) { br.skip(br.remaining()); return; }
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0) { br.skip(br.remaining()); return; }

    // Each rect: INT16 x, INT16 y, CARD16 w, CARD16 h (8 bytes)
    while (br.remaining() >= 8) {
      const int16_t  rx = (int16_t)br.readU16();
      const int16_t  ry = (int16_t)br.readU16();
      const uint16_t rw = br.readU16();
      const uint16_t rh = br.readU16();

      int32_t x0 = std::max((int32_t)rx, (int32_t)0);
      int32_t y0 = std::max((int32_t)ry, (int32_t)0);
      int32_t x1 = std::min((int32_t)rx + (int32_t)rw, (int32_t)dst.w);
      int32_t y1 = std::min((int32_t)ry + (int32_t)rh, (int32_t)dst.h);

      for (int32_t yy = y0; yy < y1; yy++) {
        uint32_t* row = dst.pixels32 + (size_t)yy * (size_t)dst.stridePixels;
        for (int32_t xx = x0; xx < x1; xx++) {
          row[(size_t)xx] = applyOp(op, row[(size_t)xx], fillColor);
        }
      }

      if (dst.isWindow) {
        damageOrDirty(ctx, dstDrawable, rx, ry, (int32_t)rw, (int32_t)rh);
      }
    }
    br.skip(br.remaining());
    return;
  }

  // ---- 27: CreateCursor ----
  case 27: {
    br.skip(br.remaining()); // stub
    return;
  }

  // ---- 28: SetPictureTransform ----
  case 28: {
    br.skip(br.remaining()); // consume silently
    return;
  }

  // ---- 29: QueryFilters ----
  case 29: {
    br.skip(br.remaining());
    // Return two built-in filters: "nearest" and "bilinear"
    static const char* filters[] = {"nearest", "bilinear"};
    static constexpr uint32_t nFilters = 2;
    static constexpr uint32_t nAliases = 0;

    // Build payload: nFilters × STR (1-byte len + chars)
    std::vector<uint8_t> payload;
    for (uint32_t i = 0; i < nFilters; i++) {
      const size_t len = std::strlen(filters[i]);
      payload.push_back((uint8_t)len);
      payload.insert(payload.end(), filters[i], filters[i] + len);
    }
    while (payload.size() % 4u) payload.push_back(0);

    const uint32_t lenWords = (uint32_t)(payload.size() / 4u);

    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      wire::wr32_le(rep.data() + 4, lenWords);
      wire::wr32_le(rep.data() + 8, nAliases);
      wire::wr32_le(rep.data() + 12, nFilters);
    });
    if (!payload.empty()) {
      ctx.reply().sendBytes(payload.data(), payload.size());
    }
    return;
  }

  // ---- 30: SetPictureFilter ----
  case 30: {
    br.skip(br.remaining()); // consume silently
    return;
  }

  // ---- 31: CreateAnimCursor ----
  case 31: {
    br.skip(br.remaining()); // stub
    return;
  }

  // ---- 33: CreateSolidFill ----
  case 33: {
    if (br.remaining() < 12) { br.skip(br.remaining()); return; }
    const uint32_t pid = br.readU32();
    const uint16_t r   = br.readU16();
    const uint16_t g   = br.readU16();
    const uint16_t b   = br.readU16();
    const uint16_t a   = br.readU16();
    br.skip(br.remaining());

    PictureState ps;
    ps.isSolid   = true;
    ps.solidARGB = renderColorToARGB(r, g, b, a);
    ps.format    = kFmtARGB32;

    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      sPictures[pid] = ps;
    }
    return;
  }

  // ---- 34-36: Gradient fills ----
  case 34: case 35: case 36: {
    // CreateLinearGradient/CreateRadialGradient/CreateConicalGradient
    // Stub: create a transparent solid fill picture
    if (br.remaining() >= 4) {
      const uint32_t pid = br.readU32();
      br.skip(br.remaining());

      PictureState ps;
      ps.isSolid   = true;
      ps.solidARGB = 0x00000000u; // transparent
      ps.format    = kFmtARGB32;

      {
        std::lock_guard<std::mutex> lk(sPicMtx);
        sPictures[pid] = ps;
      }
    } else {
      br.skip(br.remaining());
    }
    return;
  }

  default:
#ifndef NDEBUG
    fprintf(stderr, "[RENDER] unhandled minor=%u seq=%u remain=%zu\n",
            (unsigned)minor, (unsigned)seq, br.remaining());
#endif
    br.skip(br.remaining());
    return;
  }
}

} // namespace x11
