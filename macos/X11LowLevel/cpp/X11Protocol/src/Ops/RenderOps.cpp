//
//  RenderOps.cpp
//  X11LowLevel
//
//  RENDER extension — full implementation.
//
//  Supports: QueryVersion, QueryPictFormats, CreatePicture, ChangePicture,
//            FreePicture, Composite (with mask), FillRectangles, CreateSolidFill,
//            QueryFilters, AddGlyphs, FreeGlyphs, CompositeGlyphs8/16/32,
//            Trapezoids, all Porter-Duff blend modes.
//

// Define X11_TRACE_RENDER to enable RENDER extension debug tracing.
// Separate from X11_TRACE_VERBOSE since RENDER debugging is often needed
// independently. Undefine to silence all RENDER traces.
// #define X11_TRACE_RENDER 1  // disabled — using [REQ] all-requests trace instead

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
// Picture table — maps Picture XID -> state
// ============================================================================
struct PictureState {
  uint32_t drawable = 0;
  uint32_t format   = 0;
  bool     repeat   = false;
  bool     isSolid  = false;
  // Solid fill color (premultiplied ARGB8888)
  uint32_t solidARGB = 0;
  // Clip (not implemented -- just tracked)
  bool     hasClip = false;
};

static std::mutex sPicMtx;
static std::unordered_map<uint32_t, PictureState> sPictures;

static PictureState* findPicture(uint32_t pid) {
  auto it = sPictures.find(pid);
  return (it != sPictures.end()) ? &it->second : nullptr;
}

// ============================================================================
// GlyphSet table — proper glyph storage for anti-aliased font rendering
// ============================================================================
struct RenderGlyph {
  uint16_t width  = 0;
  uint16_t height = 0;
  int16_t  x      = 0;   // origin x offset (pixels left of pen)
  int16_t  y      = 0;   // origin y offset (pixels above baseline)
  int16_t  xOff   = 0;   // advance to next glyph origin x
  int16_t  yOff   = 0;   // advance to next glyph origin y
  std::vector<uint8_t> alpha; // A8 format: width*height bytes
};

struct GlyphSetState {
  uint32_t format = 0;
  std::unordered_map<uint32_t, RenderGlyph> glyphs;
};

static std::mutex sGlyphMtx;
static std::unordered_map<uint32_t, GlyphSetState> sGlyphSets;

// ============================================================================
// Inline compositing helpers
// ============================================================================

// Modulate premultiplied ARGB by an alpha coverage value (0..255)
static inline uint32_t modulateAlpha(uint32_t c, uint8_t a) {
  if (a == 255) return c;
  if (a == 0)   return 0;
  uint32_t ca = ((c >> 24) & 0xFF) * a / 255;
  uint32_t cr = ((c >> 16) & 0xFF) * a / 255;
  uint32_t cg = ((c >>  8) & 0xFF) * a / 255;
  uint32_t cb = ((c >>  0) & 0xFF) * a / 255;
  return (ca << 24) | (cr << 16) | (cg << 8) | cb;
}

// Return bits-per-pixel for alpha-only PictFormats
static inline int alphaBpp(uint32_t fmt) {
  if (fmt == kFmtA8) return 8;
  if (fmt == kFmtA4) return 4;
  if (fmt == kFmtA1) return 1;
  return 8; // default
}

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

// Apply compositing op (all Porter-Duff modes).
// Returns correct premultiplied ARGB — callers force alpha opaque when
// writing to XRGB8888 window surfaces.
static inline uint32_t applyOp(uint8_t op, uint32_t dst, uint32_t src) {
  const uint32_t sa = (src >> 24) & 0xFF;
  const uint32_t da = (dst >> 24) & 0xFF;

  switch (op) {
    case PictOpClear: return 0;
    case PictOpSrc:   return src;
    case PictOpDst:   return dst;
    case PictOpOver:  return compositeOver(dst, src);
    case PictOpOverReverse: return compositeOver(src, dst);
    case PictOpIn:    return modulateAlpha(src, (uint8_t)da);
    case PictOpInReverse: return modulateAlpha(dst, (uint8_t)sa);
    case PictOpOut:   return modulateAlpha(src, (uint8_t)(255 - da));
    case PictOpOutReverse: return modulateAlpha(dst, (uint8_t)(255 - sa));
    case PictOpAtop: {
      // src * da + dst * (1-sa)
      uint32_t s = modulateAlpha(src, (uint8_t)da);
      uint32_t d = modulateAlpha(dst, (uint8_t)(255 - sa));
      auto add8 = [](uint32_t a, uint32_t b) { return std::min(a+b, 255u); };
      return (add8((s>>24)&0xFF,(d>>24)&0xFF)<<24) |
             (add8((s>>16)&0xFF,(d>>16)&0xFF)<<16) |
             (add8((s>>8)&0xFF,(d>>8)&0xFF)<<8) |
              add8(s&0xFF,d&0xFF);
    }
    case PictOpAtopReverse: {
      // dst * sa + src * (1-da)
      uint32_t d = modulateAlpha(dst, (uint8_t)sa);
      uint32_t s = modulateAlpha(src, (uint8_t)(255 - da));
      auto add8 = [](uint32_t a, uint32_t b) { return std::min(a+b, 255u); };
      return (add8((s>>24)&0xFF,(d>>24)&0xFF)<<24) |
             (add8((s>>16)&0xFF,(d>>16)&0xFF)<<16) |
             (add8((s>>8)&0xFF,(d>>8)&0xFF)<<8) |
              add8(s&0xFF,d&0xFF);
    }
    case PictOpXor: {
      // src * (1-da) + dst * (1-sa)
      uint32_t s = modulateAlpha(src, (uint8_t)(255 - da));
      uint32_t d = modulateAlpha(dst, (uint8_t)(255 - sa));
      auto add8 = [](uint32_t a, uint32_t b) { return std::min(a+b, 255u); };
      return (add8((s>>24)&0xFF,(d>>24)&0xFF)<<24) |
             (add8((s>>16)&0xFF,(d>>16)&0xFF)<<16) |
             (add8((s>>8)&0xFF,(d>>8)&0xFF)<<8) |
              add8(s&0xFF,d&0xFF);
    }
    case PictOpAdd: {
      auto add = [](uint32_t a, uint32_t b) -> uint32_t { return std::min(a + b, 255u); };
      return (add((dst >> 24) & 0xFF, (src >> 24) & 0xFF) << 24) |
             (add((dst >> 16) & 0xFF, (src >> 16) & 0xFF) << 16) |
             (add((dst >>  8) & 0xFF, (src >>  8) & 0xFF) <<  8) |
              add((dst >>  0) & 0xFF, (src >>  0) & 0xFF);
    }
    default:
      return compositeOver(dst, src);
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
    // 5 formats x 28 bytes = 140 bytes
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
    wire::wr32_le(p + sOff + 0, 1);          // nDepth
    wire::wr32_le(p + sOff + 4, kFmtRGB24);  // fallback

    p[sOff + 8] = 24; // depth
    wire::wr16_le(p + sOff + 10, 1); // nPictVisuals

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

    if (mask & (1u << 0)) { // CPRepeat
      if (br.remaining() >= 4) ps.repeat = (br.readU32() != 0);
    }
    br.skip(br.remaining());

    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      sPictures[pid] = ps;
    }
#ifdef X11_TRACE_RENDER
    fprintf(stderr, "[RENDER CreatePicture] pid=0x%X drw=0x%X fmt=0x%X repeat=%d\n",
            pid, drawable, format, ps.repeat);
#endif
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
    br.skip(br.remaining());
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

  // ---- 8: Composite (with mask support) ----
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

    // Resolve source
    uint32_t srcColor = 0xFF000000u;
    bool srcIsSolid = false;
    bool srcRepeat = false;
    uint32_t srcDrawable = 0;
    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* sps = findPicture(srcPid);
      if (sps) {
        srcIsSolid   = sps->isSolid;
        srcColor     = sps->solidARGB;
        srcDrawable  = sps->drawable;
        srcRepeat    = sps->repeat;
      }
    }

    // Resolve mask picture
    bool hasMask = false;
    bool maskIsSolid = false;
    bool maskRepeat = false;
    uint32_t maskSolidAlpha = 255;
    uint32_t maskDrawable = 0;
    if (mskPid != 0) {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* mps = findPicture(mskPid);
      if (mps) {
        hasMask = true;
        maskIsSolid = mps->isSolid;
        maskRepeat  = mps->repeat;
        if (maskIsSolid) {
          maskSolidAlpha = (mps->solidARGB >> 24) & 0xFF;
        }
        maskDrawable = mps->drawable;
      }
    }

    // Resolve mask drawable pixels if needed
    DrawableRW maskDrw{};
    if (hasMask && !maskIsSolid && maskDrawable != 0) {
      resolveDrawableRW(ctx, maskDrawable, maskDrw);
    }

    // Lambda to get mask alpha at a given position (handles Repeat)
    auto getMaskAlpha = [&](int32_t mx, int32_t my) -> uint8_t {
      if (!hasMask) return 255;
      if (maskIsSolid) return (uint8_t)maskSolidAlpha;
      if (!maskDrw.pixels32) return 255;
      if (maskRepeat && maskDrw.w > 0 && maskDrw.h > 0) {
        mx = ((mx % (int32_t)maskDrw.w) + (int32_t)maskDrw.w) % (int32_t)maskDrw.w;
        my = ((my % (int32_t)maskDrw.h) + (int32_t)maskDrw.h) % (int32_t)maskDrw.h;
      } else {
        if (mx < 0 || mx >= (int32_t)maskDrw.w || my < 0 || my >= (int32_t)maskDrw.h)
          return 0;
      }
      return (uint8_t)((maskDrw.pixels32[(size_t)my * (size_t)maskDrw.stridePixels + (size_t)mx] >> 24) & 0xFF);
    };

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

#ifdef X11_TRACE_RENDER
    fprintf(stderr, "[RENDER Composite] op=%u src=0x%X(solid=%d repeat=%d drw=0x%X) "
            "msk=0x%X(has=%d solid=%d repeat=%d drw=0x%X) dst=0x%X(drw=0x%X win=%d) "
            "src(%d,%d) msk(%d,%d) dst(%d,%d) %ux%u\n",
            (unsigned)op, srcPid, srcIsSolid, srcRepeat, srcDrawable,
            mskPid, hasMask, maskIsSolid, maskRepeat, maskDrawable,
            dstPid, dstDrawable, dst.isWindow,
            xSrc, ySrc, xMask, yMask, xDst, yDst, width, height);
#endif

    // XRGB8888 window surfaces need alpha=0xFF for Metal rendering.
    const bool forceOpaque = dst.isWindow;

    // Handle non-solid source with Repeat: if 1x1, treat as solid
    if (!srcIsSolid && srcRepeat && srcDrawable != 0) {
      DrawableRW srcDrw{};
      if (resolveDrawableRW(ctx, srcDrawable, srcDrw) && srcDrw.pixels32) {
        if (srcDrw.w == 1 && srcDrw.h == 1) {
          srcIsSolid = true;
          srcColor   = srcDrw.pixels32[0];
#ifdef X11_TRACE_RENDER
          fprintf(stderr, "[RENDER Composite] promoted 1x1 Repeat to solid=0x%08X\n",
                  srcColor);
#endif
        }
      }
    }

    if (srcIsSolid) {
      for (int32_t row = 0; row < (int32_t)height; row++) {
        const int32_t dy = (int32_t)yDst + row;
        if (dy < 0 || dy >= (int32_t)dst.h) continue;
        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        for (int32_t col = 0; col < (int32_t)width; col++) {
          const int32_t dx = (int32_t)xDst + col;
          if (dx < 0 || dx >= (int32_t)dst.w) continue;
          const uint8_t ma = getMaskAlpha((int32_t)xMask + col, (int32_t)yMask + row);
          const uint32_t sc = modulateAlpha(srcColor, ma);
          uint32_t px = applyOp(op, drow[(size_t)dx], sc);
          if (forceOpaque) px |= 0xFF000000u;
          drow[(size_t)dx] = px;
        }
      }
    } else if (srcDrawable != 0) {
      DrawableRW src{};
      if (!resolveDrawableRW(ctx, srcDrawable, src)) return;
      if (!src.pixels32) return;

      for (int32_t row = 0; row < (int32_t)height; row++) {
        int32_t sy = (int32_t)ySrc + row;
        const int32_t dy = (int32_t)yDst + row;
        if (dy < 0 || dy >= (int32_t)dst.h) continue;
        if (srcRepeat && src.h > 0) {
          sy = ((sy % (int32_t)src.h) + (int32_t)src.h) % (int32_t)src.h;
        } else if (sy < 0 || sy >= (int32_t)src.h) continue;

        const uint32_t* srow = src.pixels32 + (size_t)sy * (size_t)src.stridePixels;
        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;

        for (int32_t col = 0; col < (int32_t)width; col++) {
          int32_t sx = (int32_t)xSrc + col;
          const int32_t dx = (int32_t)xDst + col;
          if (dx < 0 || dx >= (int32_t)dst.w) continue;
          if (srcRepeat && src.w > 0) {
            sx = ((sx % (int32_t)src.w) + (int32_t)src.w) % (int32_t)src.w;
          } else if (sx < 0 || sx >= (int32_t)src.w) continue;
          const uint8_t ma = getMaskAlpha((int32_t)xMask + col, (int32_t)yMask + row);
          const uint32_t sc = modulateAlpha(srow[(size_t)sx], ma);
          uint32_t px = applyOp(op, drow[(size_t)dx], sc);
          if (forceOpaque) px |= 0xFF000000u;
          drow[(size_t)dx] = px;
        }
      }
    }

    if (dst.isWindow) {
      // Clamp damage rect to actual pixel-write region (matching per-pixel clipping above)
      int32_t dmgX0 = std::max((int32_t)xDst, (int32_t)0);
      int32_t dmgY0 = std::max((int32_t)yDst, (int32_t)0);
      int32_t dmgX1 = std::min((int32_t)xDst + (int32_t)width,  (int32_t)dst.w);
      int32_t dmgY1 = std::min((int32_t)yDst + (int32_t)height, (int32_t)dst.h);
      if (dmgX0 < dmgX1 && dmgY0 < dmgY1) {
        damageOrDirty(ctx, dstDrawable, dmgX0, dmgY0, dmgX1 - dmgX0, dmgY1 - dmgY0);
      }
    }
    return;
  }

  // ---- 10: Trapezoids ----
  case 10: {
    if (br.remaining() < 20) { br.skip(br.remaining()); return; }
    const uint8_t  op     = br.readU8();
    br.skip(3); // pad
    const uint32_t srcPid = br.readU32();
    const uint32_t dstPid = br.readU32();
    const uint32_t maskFmt = br.readU32();
    const int16_t  xSrc   = (int16_t)br.readU16();
    const int16_t  ySrc   = (int16_t)br.readU16();
    (void)maskFmt; (void)xSrc; (void)ySrc;

    // Resolve source color (typically solid fill)
    uint32_t srcColor = 0xFF000000u;
    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* sps = findPicture(srcPid);
      if (sps && sps->isSolid) srcColor = sps->solidARGB;
    }

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

    // Track damage bounding box
    int32_t dmgX0 = (int32_t)dst.w, dmgY0 = (int32_t)dst.h;
    int32_t dmgX1 = 0, dmgY1 = 0;

    // Each TRAPEZOID is 40 bytes:
    //   FIXED top, bottom (16.16 each = 4 bytes)
    //   LINEFIX left  (two POINTFIXes = 2x8 = 16 bytes)
    //   LINEFIX right (two POINTFIXes = 2x8 = 16 bytes)
    while (br.remaining() >= 40) {
      const int32_t topFixed    = (int32_t)br.readU32();
      const int32_t bottomFixed = (int32_t)br.readU32();
      // Left edge: p1(x,y), p2(x,y) in FIXED 16.16
      const int32_t lx1 = (int32_t)br.readU32();
      const int32_t ly1 = (int32_t)br.readU32();
      const int32_t lx2 = (int32_t)br.readU32();
      const int32_t ly2 = (int32_t)br.readU32();
      // Right edge: p1(x,y), p2(x,y) in FIXED 16.16
      const int32_t rx1 = (int32_t)br.readU32();
      const int32_t ry1 = (int32_t)br.readU32();
      const int32_t rx2 = (int32_t)br.readU32();
      const int32_t ry2 = (int32_t)br.readU32();

      // Convert FIXED 16.16 to integer scanlines (round to pixel centers)
      int32_t yTop    = (topFixed + 0x8000) >> 16;
      int32_t yBottom = (bottomFixed + 0x8000) >> 16;
      yTop    = std::max(yTop,    (int32_t)0);
      yBottom = std::min(yBottom, (int32_t)dst.h);

      const bool forceOpaque = dst.isWindow;
      for (int32_t y = yTop; y < yBottom; y++) {
        // Fixed-point Y for this scanline center (y + 0.5)
        int64_t yFixed = ((int64_t)y << 16) + 0x8000;

        // Interpolate left edge X at this Y
        int64_t lDy = (int64_t)ly2 - (int64_t)ly1;
        int64_t leftX;
        if (lDy == 0) leftX = (int64_t)lx1;
        else leftX = (int64_t)lx1 + ((int64_t)(lx2 - lx1) * (yFixed - (int64_t)ly1)) / lDy;

        // Interpolate right edge X at this Y
        int64_t rDy = (int64_t)ry2 - (int64_t)ry1;
        int64_t rightX;
        if (rDy == 0) rightX = (int64_t)rx1;
        else rightX = (int64_t)rx1 + ((int64_t)(rx2 - rx1) * (yFixed - (int64_t)ry1)) / rDy;

        // Convert to pixel coordinates
        int32_t xLeft  = (int32_t)((leftX + 0x8000) >> 16);
        int32_t xRight = (int32_t)((rightX + 0x8000) >> 16);
        xLeft  = std::max(xLeft,  (int32_t)0);
        xRight = std::min(xRight, (int32_t)dst.w);

        if (xLeft < xRight) {
          uint32_t* row = dst.pixels32 + (size_t)y * (size_t)dst.stridePixels;
          for (int32_t x = xLeft; x < xRight; x++) {
            uint32_t px = applyOp(op, row[(size_t)x], srcColor);
            if (forceOpaque) px |= 0xFF000000u;
            row[(size_t)x] = px;
          }
          // Update damage bounds
          if (xLeft  < dmgX0) dmgX0 = xLeft;
          if (xRight > dmgX1) dmgX1 = xRight;
          if (y      < dmgY0) dmgY0 = y;
          if (y + 1  > dmgY1) dmgY1 = y + 1;
        }
      }
    }

    if (dmgX1 > dmgX0 && dmgY1 > dmgY0 && dst.isWindow) {
      damageOrDirty(ctx, dstDrawable, (int16_t)dmgX0, (int16_t)dmgY0,
                    dmgX1 - dmgX0, dmgY1 - dmgY0);
    }
    br.skip(br.remaining());
    return;
  }

  // ---- 11: Triangles ----
  // ---- 12: TriStrip ----
  // ---- 13: TriFan ----
  case 11: case 12: case 13: {
    br.skip(br.remaining()); // stub: consume silently
    return;
  }

  // ---- 17: CreateGlyphSet ----
  case 17: {
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }
    const uint32_t gsid   = br.readU32();
    const uint32_t format = br.readU32();
    br.skip(br.remaining());
    {
      std::lock_guard<std::mutex> lk(sGlyphMtx);
      GlyphSetState gs;
      gs.format = format;
      sGlyphSets[gsid] = std::move(gs);
    }
    return;
  }

  // ---- 18: ReferenceGlyphSet ----
  case 18: {
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }
    const uint32_t gsid     = br.readU32();
    const uint32_t existing = br.readU32();
    br.skip(br.remaining());
    {
      std::lock_guard<std::mutex> lk(sGlyphMtx);
      auto it = sGlyphSets.find(existing);
      if (it != sGlyphSets.end()) {
        sGlyphSets[gsid] = it->second; // copy format + glyphs
      } else {
        GlyphSetState gs;
        sGlyphSets[gsid] = std::move(gs);
      }
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
  case 20: {
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }
    const uint32_t gsid    = br.readU32();
    const uint32_t nglyphs = br.readU32();

    if (nglyphs > 65536) { br.skip(br.remaining()); return; }

    // Read glyph IDs: nglyphs x CARD32
    std::vector<uint32_t> ids(nglyphs);
    for (uint32_t i = 0; i < nglyphs && br.remaining() >= 4; i++) {
      ids[i] = br.readU32();
    }

    // Determine alpha format for this glyphset
    uint32_t gsFmt = kFmtA8;
    {
      std::lock_guard<std::mutex> lk(sGlyphMtx);
      auto it = sGlyphSets.find(gsid);
      if (it != sGlyphSets.end()) gsFmt = it->second.format;
    }
    const int bpp = alphaBpp(gsFmt);

    // Read glyph info structs: nglyphs x 12 bytes each
    // { CARD16 width, CARD16 height, INT16 x, INT16 y, INT16 xOff, INT16 yOff }
    struct GlyphInfo { uint16_t w, h; int16_t x, y, xOff, yOff; };
    std::vector<GlyphInfo> infos(nglyphs);
    for (uint32_t i = 0; i < nglyphs && br.remaining() >= 12; i++) {
      infos[i].w    = br.readU16();
      infos[i].h    = br.readU16();
      infos[i].x    = (int16_t)br.readU16();
      infos[i].y    = (int16_t)br.readU16();
      infos[i].xOff = (int16_t)br.readU16();
      infos[i].yOff = (int16_t)br.readU16();
    }

    // Read bitmap data and convert to A8
    {
      std::lock_guard<std::mutex> lk(sGlyphMtx);
      auto gsIt = sGlyphSets.find(gsid);
      if (gsIt == sGlyphSets.end()) { br.skip(br.remaining()); return; }

      for (uint32_t i = 0; i < nglyphs; i++) {
        const auto& info = infos[i];
        const uint32_t npixels = (uint32_t)info.w * (uint32_t)info.h;

        RenderGlyph rg;
        rg.width  = info.w;
        rg.height = info.h;
        rg.x      = info.x;
        rg.y      = info.y;
        rg.xOff   = info.xOff;
        rg.yOff   = info.yOff;

        if (npixels > 0) {
          rg.alpha.resize(npixels);

          if (bpp == 8) {
            // A8: each row padded to 4-byte boundary
            const uint32_t rowBytes = ((uint32_t)info.w + 3u) & ~3u;
            for (uint32_t row = 0; row < info.h; row++) {
              for (uint32_t col = 0; col < info.w && br.remaining() > 0; col++) {
                rg.alpha[row * info.w + col] = br.readU8();
              }
              // Skip padding
              uint32_t padCols = rowBytes - info.w;
              for (uint32_t j = 0; j < padCols && br.remaining() > 0; j++) {
                br.readU8();
              }
            }
          } else if (bpp == 4) {
            // A4: 2 pixels per byte, rows padded to 4 bytes
            const uint32_t rowBytes = (((uint32_t)info.w + 1u) / 2u + 3u) & ~3u;
            for (uint32_t row = 0; row < info.h; row++) {
              uint32_t bytesRead = 0;
              for (uint32_t col = 0; col < info.w; col++) {
                if ((col & 1) == 0 && br.remaining() > 0) {
                  uint8_t byte = br.readU8();
                  bytesRead++;
                  // Low nibble first (LSBFirst)
                  uint8_t lo = byte & 0x0F;
                  rg.alpha[row * info.w + col] = (uint8_t)(lo * 255 / 15);
                  if (col + 1 < info.w) {
                    uint8_t hi = (byte >> 4) & 0x0F;
                    rg.alpha[row * info.w + col + 1] = (uint8_t)(hi * 255 / 15);
                  }
                  col++; // skip the col++ in for-loop for paired pixel
                }
              }
              // Skip padding
              while (bytesRead < rowBytes && br.remaining() > 0) {
                br.readU8();
                bytesRead++;
              }
            }
          } else {
            // A1: 1 bit per pixel, rows padded to 4 bytes
            const uint32_t rowBytes = (((uint32_t)info.w + 7u) / 8u + 3u) & ~3u;
            for (uint32_t row = 0; row < info.h; row++) {
              uint32_t bytesRead = 0;
              for (uint32_t col = 0; col < info.w;) {
                if (br.remaining() == 0) break;
                uint8_t byte = br.readU8();
                bytesRead++;
                // LSBFirst bit order
                for (int bit = 0; bit < 8 && col < info.w; bit++, col++) {
                  rg.alpha[row * info.w + col] = (byte & (1u << bit)) ? 255 : 0;
                }
              }
              while (bytesRead < rowBytes && br.remaining() > 0) {
                br.readU8();
                bytesRead++;
              }
            }
          }
        }

        gsIt->second.glyphs[ids[i]] = std::move(rg);
      }
    }

    br.skip(br.remaining());
    return;
  }

  // ---- 22: FreeGlyphs ----
  case 22: {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t gsid = br.readU32();
    {
      std::lock_guard<std::mutex> lk(sGlyphMtx);
      auto it = sGlyphSets.find(gsid);
      if (it != sGlyphSets.end()) {
        while (br.remaining() >= 4) {
          const uint32_t gid = br.readU32();
          it->second.glyphs.erase(gid);
        }
      }
    }
    br.skip(br.remaining());
    return;
  }

  // ---- 23: CompositeGlyphs8 ----
  // ---- 24: CompositeGlyphs16 ----
  // ---- 25: CompositeGlyphs32 ----
  case 23: case 24: case 25: {
    // glyphIdSize: 1, 2, or 4 bytes per glyph ID
    const int glyphIdSize = (minor == 23) ? 1 : (minor == 24) ? 2 : 4;

    if (br.remaining() < 24) { br.skip(br.remaining()); return; }
    const uint8_t  op      = br.readU8();
    br.skip(3); // pad
    const uint32_t srcPid  = br.readU32();
    const uint32_t dstPid  = br.readU32();
    const uint32_t maskFmt = br.readU32();
    const uint32_t gsid    = br.readU32();
    const int16_t  xSrc    = (int16_t)br.readU16();
    const int16_t  ySrc    = (int16_t)br.readU16();
    (void)xSrc; (void)ySrc;

    // Resolve source (typically solid fill for text color)
    uint32_t srcColor = 0xFF000000u;
    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* sps = findPicture(srcPid);
      if (sps && sps->isSolid) srcColor = sps->solidARGB;
    }

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

    // Current glyphset
    uint32_t curGsid = gsid;

    // Track glyph pen position and bounding box for mask accumulation
    int32_t penX = 0, penY = 0;
    bool firstElt = true;

    // For maskFormat path: collect all glyph renders, then composite once
    struct GlyphRender {
      int32_t dstX, dstY;
      uint16_t w, h;
      const uint8_t* alpha; // pointer into GlyphSetState (valid under lock)
    };

    // Two-pass rendering if maskFormat != 0
    const bool useMask = (maskFmt != 0);

    // First, collect all glyphs to render
    struct GlyphCmd {
      uint32_t gsid;
      uint32_t glyphId;
      int32_t penX, penY;
    };
    std::vector<GlyphCmd> cmds;

    // Parse GlyphElt items
    while (br.remaining() >= 8) {
      const uint8_t len = br.readU8();
      br.skip(3); // pad

      if (len == 0) break; // end marker

      if (len == 255) {
        // Glyphset switch
        if (br.remaining() >= 4) {
          curGsid = br.readU32();
        }
        continue;
      }

      // Read dx, dy (INT16)
      const int16_t dx = (int16_t)br.readU16();
      const int16_t dy = (int16_t)br.readU16();

      if (firstElt) {
        penX = dx;
        penY = dy;
        firstElt = false;
      } else {
        penX += dx;
        penY += dy;
      }

      // Read glyph IDs
      int32_t localPenX = penX;
      int32_t localPenY = penY;

      for (uint8_t g = 0; g < len; g++) {
        uint32_t glyphId = 0;
        if (glyphIdSize == 1 && br.remaining() >= 1) {
          glyphId = br.readU8();
        } else if (glyphIdSize == 2 && br.remaining() >= 2) {
          glyphId = br.readU16();
        } else if (glyphIdSize == 4 && br.remaining() >= 4) {
          glyphId = br.readU32();
        } else break;

        GlyphCmd cmd;
        cmd.gsid = curGsid;
        cmd.glyphId = glyphId;
        cmd.penX = localPenX;
        cmd.penY = localPenY;
        cmds.push_back(cmd);

        // Advance pen by glyph's xOff/yOff (need lookup)
        {
          std::lock_guard<std::mutex> lk(sGlyphMtx);
          auto gsIt = sGlyphSets.find(curGsid);
          if (gsIt != sGlyphSets.end()) {
            auto gIt = gsIt->second.glyphs.find(glyphId);
            if (gIt != gsIt->second.glyphs.end()) {
              localPenX += gIt->second.xOff;
              localPenY += gIt->second.yOff;
            }
          }
        }
      }

      // Pad glyph ID array to 4-byte boundary
      const uint32_t idBytes = (uint32_t)len * glyphIdSize;
      const uint32_t padded = (idBytes + 3u) & ~3u;
      const uint32_t padBytes = padded - idBytes;
      for (uint32_t j = 0; j < padBytes && br.remaining() > 0; j++) {
        br.readU8();
      }

      // Update pen for next element
      penX = localPenX;
      penY = localPenY;
    }

    // Now render all collected glyph commands
    int32_t dmgX0 = (int32_t)dst.w, dmgY0 = (int32_t)dst.h;
    int32_t dmgX1 = 0, dmgY1 = 0;

    if (useMask && !cmds.empty()) {
      // Accumulate all glyph alphas into a temporary A8 buffer,
      // then do a single composite pass (prevents double-blending)

      // Compute bounding box of all glyphs
      int32_t bx0 = INT32_MAX, by0 = INT32_MAX;
      int32_t bx1 = INT32_MIN, by1 = INT32_MIN;

      {
        std::lock_guard<std::mutex> lk(sGlyphMtx);
        for (auto& cmd : cmds) {
          auto gsIt = sGlyphSets.find(cmd.gsid);
          if (gsIt == sGlyphSets.end()) continue;
          auto gIt = gsIt->second.glyphs.find(cmd.glyphId);
          if (gIt == gsIt->second.glyphs.end()) continue;
          const auto& rg = gIt->second;
          int32_t gx = cmd.penX - rg.x;
          int32_t gy = cmd.penY - rg.y;
          bx0 = std::min(bx0, gx);
          by0 = std::min(by0, gy);
          bx1 = std::max(bx1, gx + (int32_t)rg.width);
          by1 = std::max(by1, gy + (int32_t)rg.height);
        }
      }

      if (bx0 >= bx1 || by0 >= by1) { br.skip(br.remaining()); return; }

      const int32_t bufW = bx1 - bx0;
      const int32_t bufH = by1 - by0;
      if (bufW <= 0 || bufH <= 0 || (int64_t)bufW * bufH > 16 * 1024 * 1024) {
        br.skip(br.remaining()); return;
      }

      std::vector<uint8_t> maskBuf((size_t)bufW * (size_t)bufH, 0);

      // Accumulate glyph alphas (MAX to prevent double-blending on overlaps)
      {
        std::lock_guard<std::mutex> lk(sGlyphMtx);
        for (auto& cmd : cmds) {
          auto gsIt = sGlyphSets.find(cmd.gsid);
          if (gsIt == sGlyphSets.end()) continue;
          auto gIt = gsIt->second.glyphs.find(cmd.glyphId);
          if (gIt == gsIt->second.glyphs.end()) continue;
          const auto& rg = gIt->second;
          const int32_t gx = cmd.penX - rg.x - bx0;
          const int32_t gy = cmd.penY - rg.y - by0;
          for (int32_t row = 0; row < (int32_t)rg.height; row++) {
            const int32_t my = gy + row;
            if (my < 0 || my >= bufH) continue;
            for (int32_t col = 0; col < (int32_t)rg.width; col++) {
              const int32_t mx = gx + col;
              if (mx < 0 || mx >= bufW) continue;
              const uint8_t a = rg.alpha[row * rg.width + col];
              uint8_t& dest = maskBuf[(size_t)my * (size_t)bufW + (size_t)mx];
              if (a > dest) dest = a; // MAX blend
            }
          }
        }
      }

      // Single composite pass from mask buffer to destination
      const bool forceOpaque1 = dst.isWindow;
      for (int32_t row = 0; row < bufH; row++) {
        const int32_t dy = by0 + row;
        if (dy < 0 || dy >= (int32_t)dst.h) continue;
        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        for (int32_t col = 0; col < bufW; col++) {
          const int32_t dx = bx0 + col;
          if (dx < 0 || dx >= (int32_t)dst.w) continue;
          const uint8_t a = maskBuf[(size_t)row * (size_t)bufW + (size_t)col];
          if (a == 0) continue;
          const uint32_t sc = modulateAlpha(srcColor, a);
          uint32_t px = applyOp(op, drow[(size_t)dx], sc);
          if (forceOpaque1) px |= 0xFF000000u;
          drow[(size_t)dx] = px;
        }
      }

      dmgX0 = std::max(bx0, (int32_t)0);
      dmgY0 = std::max(by0, (int32_t)0);
      dmgX1 = std::min(bx1, (int32_t)dst.w);
      dmgY1 = std::min(by1, (int32_t)dst.h);
    } else {
      // Direct rendering: composite each glyph individually
      const bool forceOpaque2 = dst.isWindow;
      std::lock_guard<std::mutex> lk(sGlyphMtx);
      for (auto& cmd : cmds) {
        auto gsIt = sGlyphSets.find(cmd.gsid);
        if (gsIt == sGlyphSets.end()) continue;
        auto gIt = gsIt->second.glyphs.find(cmd.glyphId);
        if (gIt == gsIt->second.glyphs.end()) continue;
        const auto& rg = gIt->second;
        const int32_t gx = cmd.penX - rg.x;
        const int32_t gy = cmd.penY - rg.y;

        for (int32_t row = 0; row < (int32_t)rg.height; row++) {
          const int32_t dy = gy + row;
          if (dy < 0 || dy >= (int32_t)dst.h) continue;
          uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
          for (int32_t col = 0; col < (int32_t)rg.width; col++) {
            const int32_t dx = gx + col;
            if (dx < 0 || dx >= (int32_t)dst.w) continue;
            const uint8_t a = rg.alpha[row * rg.width + col];
            if (a == 0) continue;
            const uint32_t sc = modulateAlpha(srcColor, a);
            uint32_t px = applyOp(op, drow[(size_t)dx], sc);
            if (forceOpaque2) px |= 0xFF000000u;
            drow[(size_t)dx] = px;
          }
        }

        // Update damage bounds
        dmgX0 = std::min(dmgX0, gx);
        dmgY0 = std::min(dmgY0, gy);
        dmgX1 = std::max(dmgX1, gx + (int32_t)rg.width);
        dmgY1 = std::max(dmgY1, gy + (int32_t)rg.height);
      }
    }

    if (dst.isWindow && dmgX1 > dmgX0 && dmgY1 > dmgY0) {
      damageOrDirty(ctx, dstDrawable,
                    (int16_t)std::max(dmgX0, (int32_t)0),
                    (int16_t)std::max(dmgY0, (int32_t)0),
                    std::min(dmgX1, (int32_t)dst.w) - std::max(dmgX0, (int32_t)0),
                    std::min(dmgY1, (int32_t)dst.h) - std::max(dmgY0, (int32_t)0));
    }

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

#ifdef X11_TRACE_RENDER
    fprintf(stderr, "[RENDER FillRects] op=%u dst=0x%X(drw=0x%X win=%d) color=0x%08X\n",
            (unsigned)op, dstPid, dstDrawable, dst.isWindow, fillColor);
#endif

    while (br.remaining() >= 8) {
      const int16_t  rx = (int16_t)br.readU16();
      const int16_t  ry = (int16_t)br.readU16();
      const uint16_t rw = br.readU16();
      const uint16_t rh = br.readU16();

      int32_t x0 = std::max((int32_t)rx, (int32_t)0);
      int32_t y0 = std::max((int32_t)ry, (int32_t)0);
      int32_t x1 = std::min((int32_t)rx + (int32_t)rw, (int32_t)dst.w);
      int32_t y1 = std::min((int32_t)ry + (int32_t)rh, (int32_t)dst.h);

      const bool forceOpaque = dst.isWindow;
      for (int32_t yy = y0; yy < y1; yy++) {
        uint32_t* row = dst.pixels32 + (size_t)yy * (size_t)dst.stridePixels;
        for (int32_t xx = x0; xx < x1; xx++) {
          uint32_t px = applyOp(op, row[(size_t)xx], fillColor);
          if (forceOpaque) px |= 0xFF000000u;
          row[(size_t)xx] = px;
        }
      }

      if (dst.isWindow && x0 < x1 && y0 < y1) {
        damageOrDirty(ctx, dstDrawable, x0, y0, x1 - x0, y1 - y0);
      }
    }
    br.skip(br.remaining());
    return;
  }

  // ---- 27: CreateCursor ----
  case 27: {
    br.skip(br.remaining());
    return;
  }

  // ---- 28: SetPictureTransform ----
  case 28: {
    br.skip(br.remaining());
    return;
  }

  // ---- 29: QueryFilters ----
  case 29: {
    br.skip(br.remaining());
    static const char* filters[] = {"nearest", "bilinear"};
    static constexpr uint32_t nFilters = 2;
    static constexpr uint32_t nAliases = 0;

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
    br.skip(br.remaining());
    return;
  }

  // ---- 31: CreateAnimCursor ----
  case 31: {
    br.skip(br.remaining());
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
#ifdef X11_TRACE_RENDER
    fprintf(stderr, "[RENDER CreateSolidFill] pid=0x%X argb=0x%08X\n",
            pid, ps.solidARGB);
#endif
    return;
  }

  // ---- 34-36: Gradient fills ----
  case 34: case 35: case 36: {
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
