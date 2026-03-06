//
//  RenderOps.cpp
//  X11LowLevel
//
//  RENDER extension — full implementation.
//
//  Supports: QueryVersion, QueryPictFormats, CreatePicture, ChangePicture,
//            FreePicture, Composite (with mask + gradient sources),
//            FillRectangles, CreateSolidFill, QueryFilters,
//            AddGlyphs, FreeGlyphs, CompositeGlyphs8/16/32,
//            Trapezoids, Triangles/TriStrip/TriFan,
//            CreateLinearGradient, CreateRadialGradient, CreateConicalGradient,
//            all Porter-Duff blend modes.
//

#include "Utils/TraceDefs.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>

#include "Ops/RenderOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Transport/XProtoTransport.hpp"
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
// Gradient data — stored in Picture for gradient fill sources
// ============================================================================
enum GradientType : uint8_t { GradLinear = 1, GradRadial = 2, GradConical = 3 };

struct GradientStop {
  float    position; // 0.0 to 1.0 (from FIXED 16.16)
  uint32_t color;    // premultiplied ARGB8888
};

struct GradientData {
  GradientType type = GradLinear;
  // Linear: p1→p2.  Radial: inner center, outer center.  Conical: center in p1.
  float p1x = 0, p1y = 0;
  float p2x = 0, p2y = 0;
  float r1 = 0, r2 = 0;   // radial inner/outer radius
  float angle = 0;          // conical start angle (radians)
  std::vector<GradientStop> stops;
};

// Interpolate between two ARGB8888 colors
static inline uint32_t lerpColor(uint32_t c0, uint32_t c1, float frac) {
  if (frac <= 0.0f) return c0;
  if (frac >= 1.0f) return c1;
  auto l = [frac](uint32_t a, uint32_t b) -> uint32_t {
    return (uint32_t)((float)a + frac * ((float)b - (float)a) + 0.5f);
  };
  return (l((c0>>24)&0xFF, (c1>>24)&0xFF) << 24)
       | (l((c0>>16)&0xFF, (c1>>16)&0xFF) << 16)
       | (l((c0>>8)&0xFF,  (c1>>8)&0xFF)  <<  8)
       |  l( c0    &0xFF,   c1    &0xFF);
}

// Sample gradient at parameter t, with Pad (clamp) behaviour
static uint32_t sampleGradientStops(const std::vector<GradientStop>& stops, float t) {
  if (stops.empty()) return 0xFF000000u;
  if (stops.size() == 1) return stops[0].color;
  t = std::max(0.0f, std::min(1.0f, t));
  if (t <= stops.front().position) return stops.front().color;
  if (t >= stops.back().position)  return stops.back().color;
  for (size_t i = 0; i + 1 < stops.size(); i++) {
    if (t >= stops[i].position && t <= stops[i+1].position) {
      float range = stops[i+1].position - stops[i].position;
      if (range < 1e-6f) return stops[i].color;
      return lerpColor(stops[i].color, stops[i+1].color,
                       (t - stops[i].position) / range);
    }
  }
  return stops.back().color;
}

// Sample a gradient at source-coordinate (sx, sy)
static uint32_t sampleGradient(const GradientData& grad, float sx, float sy) {
  float t = 0.0f;
  switch (grad.type) {
    case GradLinear: {
      float dx = grad.p2x - grad.p1x;
      float dy = grad.p2y - grad.p1y;
      float len2 = dx*dx + dy*dy;
      if (len2 < 1e-6f) t = 0.0f;
      else t = ((sx - grad.p1x)*dx + (sy - grad.p1y)*dy) / len2;
      break;
    }
    case GradRadial: {
      // Simplified: if inner center == outer center, concentric circles
      // Otherwise, linear interpolation between inner and outer circles
      float dx = sx - grad.p1x;
      float dy = sy - grad.p1y;
      float dist = sqrtf(dx*dx + dy*dy);
      float dr = grad.r2 - grad.r1;
      if (fabsf(dr) < 1e-6f) t = (dist < grad.r1) ? 0.0f : 1.0f;
      else t = (dist - grad.r1) / dr;
      break;
    }
    case GradConical: {
      float dx = sx - grad.p1x;
      float dy = sy - grad.p1y;
      float a = atan2f(dy, dx) - grad.angle;
      t = a / (2.0f * (float)M_PI);
      t = t - floorf(t); // normalize to [0, 1)
      break;
    }
  }
  return sampleGradientStops(grad.stops, t);
}

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
  // Gradient source (non-null when this picture is a gradient fill)
  std::shared_ptr<GradientData> gradient;
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
  if (fmt == kFmtARGB32) return 32;
  if (fmt == kFmtRGB24)  return 32; // wire format is 32bpp (padded)
  if (fmt == kFmtA8) return 8;
  if (fmt == kFmtA4) return 4;
  if (fmt == kFmtA1) return 1;
  return 8; // default
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
#if X11_TRACE_RENDER_ENABLED
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
    std::shared_ptr<GradientData> srcGrad;
    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* sps = findPicture(srcPid);
      if (sps) {
        srcIsSolid   = sps->isSolid;
        srcColor     = sps->solidARGB;
        srcDrawable  = sps->drawable;
        srcRepeat    = sps->repeat;
        srcGrad      = sps->gradient;
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

#if X11_TRACE_RENDER_ENABLED
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
#if X11_TRACE_RENDER_ENABLED
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
    } else if (srcGrad) {
      // Gradient source: sample per-pixel
      for (int32_t row = 0; row < (int32_t)height; row++) {
        const int32_t dy = (int32_t)yDst + row;
        if (dy < 0 || dy >= (int32_t)dst.h) continue;
        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        const float sy = (float)(ySrc + row);
        for (int32_t col = 0; col < (int32_t)width; col++) {
          const int32_t dx = (int32_t)xDst + col;
          if (dx < 0 || dx >= (int32_t)dst.w) continue;
          const float sx = (float)(xSrc + col);
          uint32_t gradColor = sampleGradient(*srcGrad, sx, sy);
          const uint8_t ma = getMaskAlpha((int32_t)xMask + col, (int32_t)yMask + row);
          const uint32_t sc = modulateAlpha(gradColor, ma);
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

  // ---- 10: Trapezoids (with sub-pixel coverage antialiasing) ----
  case 10: {
    if (br.remaining() < 20) { br.skip(br.remaining()); return; }
    const uint8_t  op      = br.readU8();
    br.skip(3); // pad
    const uint32_t srcPid  = br.readU32();
    const uint32_t dstPid  = br.readU32();
    const uint32_t maskFmt = br.readU32();
    const int16_t  xSrc    = (int16_t)br.readU16();
    const int16_t  ySrc    = (int16_t)br.readU16();
    (void)xSrc; (void)ySrc; // used for source offset (solid fill = no effect)

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

    // Parse ALL trapezoids into a vector (ByteReader is single-pass).
    // Each TRAPEZOID is 40 bytes:
    //   FIXED top, bottom (16.16 each = 4 bytes)
    //   LINEFIX left  (two POINTFIXes = 2x8 = 16 bytes)
    //   LINEFIX right (two POINTFIXes = 2x8 = 16 bytes)
    struct ParsedTrap {
      int32_t topFixed, bottomFixed;
      int32_t lx1, ly1, lx2, ly2;   // left edge endpoints  (FIXED 16.16)
      int32_t rx1, ry1, rx2, ry2;   // right edge endpoints (FIXED 16.16)
    };
    std::vector<ParsedTrap> traps;
    traps.reserve(br.remaining() / 40);
    while (br.remaining() >= 40) {
      ParsedTrap t;
      t.topFixed    = (int32_t)br.readU32();
      t.bottomFixed = (int32_t)br.readU32();
      t.lx1 = (int32_t)br.readU32(); t.ly1 = (int32_t)br.readU32();
      t.lx2 = (int32_t)br.readU32(); t.ly2 = (int32_t)br.readU32();
      t.rx1 = (int32_t)br.readU32(); t.ry1 = (int32_t)br.readU32();
      t.rx2 = (int32_t)br.readU32(); t.ry2 = (int32_t)br.readU32();
      traps.push_back(t);
    }
    br.skip(br.remaining());
    if (traps.empty()) return;

#if X11_TRACE_RENDER_ENABLED
    fprintf(stderr, "[RENDER Trapezoids] op=%u src=0x%X dst=0x%X maskFmt=0x%X nTraps=%zu\n",
            (unsigned)op, srcPid, dstPid, maskFmt, traps.size());
#endif

    // ---------------------------------------------------------------
    // Sub-pixel coverage antialiasing constants.
    // N_SUB Y sub-rows per pixel row; analytical horizontal coverage.
    // ---------------------------------------------------------------
    constexpr int N_SUB = 8;
    // Scale factor: each sub-row contributes up to 255/N_SUB coverage units.
    // We accumulate fixed-point coverage and convert to 0..255 at the end.

    // Lambda: interpolate edge X at a given yFixed (FIXED 16.16).
    // Returns X in FIXED 16.16.
    auto interpEdgeX = [](int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                          int64_t yF) -> int64_t {
      int64_t dy = (int64_t)y2 - (int64_t)y1;
      if (dy == 0) return (int64_t)x1;
      return (int64_t)x1 + ((int64_t)(x2 - x1) * (yF - (int64_t)y1)) / dy;
    };

    // Lambda: rasterize one trapezoid into a coverage row buffer.
    // For each pixel row, iterates N_SUB sub-rows, computing analytical
    // horizontal coverage per pixel and accumulating into covRow[].
    //
    // covRow is caller-provided scratch, size >= clipW.
    // Writes coverage to either coverageBuf (mask path) or composites
    // directly to dst (direct path).
    auto rasterOneTrap = [&](const ParsedTrap& t,
                             uint8_t* coverageBuf,
                             int32_t bufW, int32_t bufOffX, int32_t bufOffY,
                             int32_t clipW, int32_t clipH,
                             int32_t* covRow,      // scratch, size >= clipW
                             bool directComposite,
                             int32_t& dmgX0, int32_t& dmgY0,
                             int32_t& dmgX1, int32_t& dmgY1)
    {
      // Pixel-row range for this trapezoid
      int32_t yTop    = t.topFixed >> 16;                      // floor
      int32_t yBottom = (t.bottomFixed + 0xFFFF) >> 16;        // ceil
      yTop    = std::max(yTop, (int32_t)0);
      yBottom = std::min(yBottom, clipH);

      const bool forceOpaque = dst.isWindow;

      for (int32_t y = yTop; y < yBottom; y++) {
        // Track X bounds across all sub-rows for this pixel row
        int32_t xMin = clipW, xMax = 0;

        // Zero the coverage row scratch (only the portion we'll use)
        // We'll track xMin/xMax to zero only what's needed.
        bool rowUsed = false;

        for (int sub = 0; sub < N_SUB; sub++) {
          // Sub-row Y center: y + (sub + 0.5) / N_SUB  in FIXED 16.16
          int64_t yF = ((int64_t)y << 16) + ((int64_t)(2 * sub + 1) << 15) / N_SUB;

          // Skip sub-rows outside the trapezoid Y extent
          if (yF < (int64_t)t.topFixed || yF >= (int64_t)t.bottomFixed)
            continue;

          // Interpolate edge X positions at this sub-row
          int64_t leftX  = interpEdgeX(t.lx1, t.ly1, t.lx2, t.ly2, yF);
          int64_t rightX = interpEdgeX(t.rx1, t.ry1, t.rx2, t.ry2, yF);
          if (leftX > rightX) std::swap(leftX, rightX);

          // Integer pixel column range (expand by 1 for partial edge pixels)
          int32_t pxL = (int32_t)(leftX >> 16);          // floor of left edge
          int32_t pxR = (int32_t)((rightX + 0xFFFF) >> 16); // ceil of right edge
          pxL = std::max(pxL, (int32_t)0);
          pxR = std::min(pxR, clipW);
          if (pxL >= pxR) continue;

          // On first valid sub-row for this pixel row, zero the scratch
          if (!rowUsed) {
            for (int32_t i = pxL; i < pxR; i++) covRow[i] = 0;
            xMin = pxL;
            xMax = pxR;
            rowUsed = true;
          } else {
            // Expand and zero new pixels if sub-row span is wider
            if (pxL < xMin) {
              for (int32_t i = pxL; i < xMin; i++) covRow[i] = 0;
              xMin = pxL;
            }
            if (pxR > xMax) {
              for (int32_t i = xMax; i < pxR; i++) covRow[i] = 0;
              xMax = pxR;
            }
          }

          // Accumulate horizontal coverage for each pixel in the span.
          // Coverage = fraction of pixel width [x, x+1) covered by [leftX, rightX],
          // scaled to 0..65536 (one full pixel width in FIXED 16.16).
          for (int32_t x = pxL; x < pxR; x++) {
            int64_t pixL = (int64_t)x << 16;
            int64_t pixR = ((int64_t)x + 1) << 16;
            int64_t covL = std::max(leftX, pixL);
            int64_t covR = std::min(rightX, pixR);
            int64_t covW = covR - covL;
            if (covW <= 0) continue;
            // covW is in [0, 0x10000]. Scale to [0, 255]:
            int32_t subCov = (int32_t)((covW * 255 + 0x8000) >> 16);
            covRow[x] += subCov;
          }
        } // sub-rows

        if (!rowUsed) continue;

        // Convert accumulated coverage to 0..255 (divide by N_SUB)
        // and either write to coverage buffer or composite directly.
        for (int32_t x = xMin; x < xMax; x++) {
          int32_t avg = (covRow[x] + N_SUB / 2) / N_SUB;
          if (avg <= 0) continue;
          uint8_t cov = (uint8_t)std::min(avg, 255);

          if (!directComposite) {
            // maskFormat != 0: accumulate into coverage buffer (saturating add)
            size_t idx = (size_t)(y - bufOffY) * (size_t)bufW + (size_t)(x - bufOffX);
            int32_t v = (int32_t)coverageBuf[idx] + (int32_t)cov;
            coverageBuf[idx] = (uint8_t)std::min(v, 255);
          } else {
            // maskFormat == 0: composite directly
            uint32_t* row = dst.pixels32 + (size_t)y * (size_t)dst.stridePixels;
            uint32_t sc = modulateAlpha(srcColor, cov);
            uint32_t px = applyOp(op, row[(size_t)x], sc);
            if (forceOpaque) px |= 0xFF000000u;
            row[(size_t)x] = px;
          }
        }

        // Update damage bounds
        if (xMin < dmgX0) dmgX0 = xMin;
        if (xMax > dmgX1) dmgX1 = xMax;
        if (y    < dmgY0) dmgY0 = y;
        if (y +1 > dmgY1) dmgY1 = y + 1;
      } // pixel rows
    }; // rasterOneTrap

    // Track damage bounding box
    int32_t dmgX0 = (int32_t)dst.w, dmgY0 = (int32_t)dst.h;
    int32_t dmgX1 = 0, dmgY1 = 0;

    // Scratch buffer for per-row coverage accumulation (reused across rows)
    std::vector<int32_t> covRow((size_t)dst.w, 0);

    if (maskFmt != 0) {
      // ---- maskFormat != 0 (typically A8): grouped compositing via temp mask ----
      // Compute bounding box of ALL trapezoids
      int32_t bbX0 = (int32_t)dst.w, bbY0 = (int32_t)dst.h;
      int32_t bbX1 = 0,               bbY1 = 0;
      for (const auto& t : traps) {
        int32_t yT = t.topFixed >> 16;
        int32_t yB = (t.bottomFixed + 0xFFFF) >> 16;
        int32_t xL = std::min({t.lx1 >> 16, t.lx2 >> 16, t.rx1 >> 16, t.rx2 >> 16});
        int32_t xR = (std::max({t.lx1, t.lx2, t.rx1, t.rx2}) + 0xFFFF) >> 16;
        bbX0 = std::min(bbX0, xL);  bbX1 = std::max(bbX1, xR);
        bbY0 = std::min(bbY0, yT);  bbY1 = std::max(bbY1, yB);
      }
      bbX0 = std::max(bbX0, (int32_t)0);
      bbY0 = std::max(bbY0, (int32_t)0);
      bbX1 = std::min(bbX1, (int32_t)dst.w);
      bbY1 = std::min(bbY1, (int32_t)dst.h);

      if (bbX0 < bbX1 && bbY0 < bbY1) {
        const int32_t bufW = bbX1 - bbX0;
        const int32_t bufH = bbY1 - bbY0;
        const size_t  bufSz = (size_t)bufW * (size_t)bufH;

        // Safety: cap coverage buffer at 4MB to prevent malicious OOM
        if (bufSz <= 4u * 1024u * 1024u) {
          std::vector<uint8_t> coverageBuf(bufSz, 0);

          // Rasterize all trapezoids into coverage buffer (additive)
          for (const auto& t : traps) {
            rasterOneTrap(t, coverageBuf.data(), bufW, bbX0, bbY0,
                          (int32_t)dst.w, (int32_t)dst.h,
                          covRow.data(), /*directComposite=*/false,
                          dmgX0, dmgY0, dmgX1, dmgY1);
          }

          // Composite coverage buffer onto destination in one pass
          const bool forceOpaque = dst.isWindow;
          for (int32_t y = bbY0; y < bbY1; y++) {
            uint32_t*      drow = dst.pixels32 + (size_t)y * (size_t)dst.stridePixels;
            const uint8_t* crow = coverageBuf.data() + (size_t)(y - bbY0) * (size_t)bufW;
            for (int32_t x = bbX0; x < bbX1; x++) {
              uint8_t cov = crow[x - bbX0];
              if (cov == 0) continue;
              uint32_t sc = modulateAlpha(srcColor, cov);
              uint32_t px = applyOp(op, drow[(size_t)x], sc);
              if (forceOpaque) px |= 0xFF000000u;
              drow[(size_t)x] = px;
            }
          }
          dmgX0 = bbX0; dmgY0 = bbY0;
          dmgX1 = bbX1; dmgY1 = bbY1;
        } else {
          // Buffer too large: fall back to direct per-trapezoid compositing
          for (const auto& t : traps) {
            rasterOneTrap(t, nullptr, 0, 0, 0,
                          (int32_t)dst.w, (int32_t)dst.h,
                          covRow.data(), /*directComposite=*/true,
                          dmgX0, dmgY0, dmgX1, dmgY1);
          }
        }
      }
    } else {
      // ---- maskFormat == 0: direct per-trapezoid compositing ----
      for (const auto& t : traps) {
        rasterOneTrap(t, nullptr, 0, 0, 0,
                      (int32_t)dst.w, (int32_t)dst.h,
                      covRow.data(), /*directComposite=*/true,
                      dmgX0, dmgY0, dmgX1, dmgY1);
      }
    }

    if (dmgX1 > dmgX0 && dmgY1 > dmgY0 && dst.isWindow) {
      damageOrDirty(ctx, dstDrawable, (int16_t)dmgX0, (int16_t)dmgY0,
                    dmgX1 - dmgX0, dmgY1 - dmgY0);
    }
    return;
  }

  // ---- 11: Triangles ----
  // ---- 12: TriStrip ----
  // ---- 13: TriFan ----
  case 11: case 12: case 13: {
    if (br.remaining() < 20) { br.skip(br.remaining()); return; }
    const uint8_t  triOp   = br.readU8();
    br.skip(3); // pad
    const uint32_t triSrcPid = br.readU32();
    const uint32_t triDstPid = br.readU32();
    const uint32_t triMaskFmt = br.readU32();
    const int16_t  triXSrc   = (int16_t)br.readU16();
    const int16_t  triYSrc   = (int16_t)br.readU16();
    (void)triMaskFmt; (void)triXSrc; (void)triYSrc;

    // Resolve source color (solid fill)
    uint32_t triSrcColor = 0xFF000000u;
    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* sps = findPicture(triSrcPid);
      if (sps && sps->isSolid) triSrcColor = sps->solidARGB;
    }

    // Resolve destination
    uint32_t triDstDrawable = 0;
    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* dps = findPicture(triDstPid);
      if (dps) triDstDrawable = dps->drawable;
    }
    if (triDstDrawable == 0) { br.skip(br.remaining()); return; }

    DrawableRW triDst{};
    if (!resolveDrawableRW(ctx, triDstDrawable, triDst)) { br.skip(br.remaining()); return; }
    if (!triDst.pixels32 || triDst.w == 0 || triDst.h == 0) { br.skip(br.remaining()); return; }

    int32_t tdmgX0 = (int32_t)triDst.w, tdmgY0 = (int32_t)triDst.h;
    int32_t tdmgX1 = 0, tdmgY1 = 0;
    const bool triForceOpaque = triDst.isWindow;

    // Read all POINTFIX values (each is 8 bytes: FIXED x, FIXED y)
    std::vector<std::pair<int32_t, int32_t>> triPts;
    triPts.reserve(br.remaining() / 8);
    while (br.remaining() >= 8) {
      int32_t px = (int32_t)br.readU32();
      int32_t py = (int32_t)br.readU32();
      triPts.push_back({px, py});
    }

    // Scanline-rasterize a single triangle (vertices in FIXED 16.16)
    auto rasterTri = [&](int32_t ax, int32_t ay, int32_t bx, int32_t by,
                         int32_t cx, int32_t cy) {
      // Sort vertices by Y (ascending)
      if (ay > by) { std::swap(ax, bx); std::swap(ay, by); }
      if (ay > cy) { std::swap(ax, cx); std::swap(ay, cy); }
      if (by > cy) { std::swap(bx, cx); std::swap(by, cy); }

      // Convert FIXED 16.16 to integer scanlines
      int32_t yTop = (ay + 0x8000) >> 16;
      int32_t yMid = (by + 0x8000) >> 16;
      int32_t yBot = (cy + 0x8000) >> 16;
      yTop = std::max(yTop, (int32_t)0);
      yBot = std::min(yBot, (int32_t)triDst.h);
      yMid = std::clamp(yMid, yTop, yBot);

      int64_t acDy = (int64_t)cy - (int64_t)ay;
      int64_t abDy = (int64_t)by - (int64_t)ay;
      int64_t bcDy = (int64_t)cy - (int64_t)by;

      for (int32_t y = yTop; y < yBot; y++) {
        int64_t yFixed = ((int64_t)y << 16) + 0x8000;

        // Long edge a→c
        int64_t xAC;
        if (acDy == 0) xAC = (int64_t)ax;
        else xAC = (int64_t)ax + ((int64_t)(cx - ax) * (yFixed - (int64_t)ay)) / acDy;

        // Short edge: a→b (upper) or b→c (lower)
        int64_t xOther;
        if (y < yMid || (abDy > 0 && yFixed < (int64_t)by)) {
          if (abDy == 0) xOther = (int64_t)ax;
          else xOther = (int64_t)ax + ((int64_t)(bx - ax) * (yFixed - (int64_t)ay)) / abDy;
        } else {
          if (bcDy == 0) xOther = (int64_t)bx;
          else xOther = (int64_t)bx + ((int64_t)(cx - bx) * (yFixed - (int64_t)by)) / bcDy;
        }

        int32_t xLeft  = (int32_t)((std::min(xAC, xOther) + 0x8000) >> 16);
        int32_t xRight = (int32_t)((std::max(xAC, xOther) + 0x8000) >> 16);
        xLeft  = std::max(xLeft,  (int32_t)0);
        xRight = std::min(xRight, (int32_t)triDst.w);

        if (xLeft < xRight) {
          uint32_t* row = triDst.pixels32 + (size_t)y * (size_t)triDst.stridePixels;
          for (int32_t x = xLeft; x < xRight; x++) {
            uint32_t px = applyOp(triOp, row[(size_t)x], triSrcColor);
            if (triForceOpaque) px |= 0xFF000000u;
            row[(size_t)x] = px;
          }
          if (xLeft  < tdmgX0) tdmgX0 = xLeft;
          if (xRight > tdmgX1) tdmgX1 = xRight;
          if (y      < tdmgY0) tdmgY0 = y;
          if (y + 1  > tdmgY1) tdmgY1 = y + 1;
        }
      }
    };

    // Generate triangles based on minor opcode
    if (minor == 11) {
      // Triangles: every 3 points form an independent triangle
      for (size_t i = 0; i + 2 < triPts.size(); i += 3) {
        rasterTri(triPts[i].first, triPts[i].second,
                  triPts[i+1].first, triPts[i+1].second,
                  triPts[i+2].first, triPts[i+2].second);
      }
    } else if (minor == 12) {
      // TriStrip: each new point adds a triangle with previous two
      for (size_t i = 0; i + 2 < triPts.size(); i++) {
        rasterTri(triPts[i].first, triPts[i].second,
                  triPts[i+1].first, triPts[i+1].second,
                  triPts[i+2].first, triPts[i+2].second);
      }
    } else { // minor == 13
      // TriFan: all triangles share the first point
      for (size_t i = 1; i + 1 < triPts.size(); i++) {
        rasterTri(triPts[0].first, triPts[0].second,
                  triPts[i].first, triPts[i].second,
                  triPts[i+1].first, triPts[i+1].second);
      }
    }

    if (tdmgX1 > tdmgX0 && tdmgY1 > tdmgY0 && triDst.isWindow) {
      damageOrDirty(ctx, triDstDrawable, (int16_t)tdmgX0, (int16_t)tdmgY0,
                    tdmgX1 - tdmgX0, tdmgY1 - tdmgY0);
    }
    br.skip(br.remaining());
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
#if X11_TRACE_RENDER_ENABLED
    {
      const char* fmtName = (format == kFmtARGB32) ? "ARGB32" :
                             (format == kFmtRGB24) ? "RGB24" :
                             (format == kFmtA8) ? "A8" :
                             (format == kFmtA4) ? "A4" :
                             (format == kFmtA1) ? "A1" : "?";
      fprintf(stderr, "[RENDER] CreateGlyphSet gsid=0x%x format=0x%x (%s)\n",
              gsid, format, fmtName);
    }
#endif
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

          if (bpp == 32) {
            // ARGB32/RGB24: 4 bytes per pixel, rows naturally 4-byte aligned.
            // Extract alpha channel for compositing (loses subpixel coverage
            // for LCD/component-alpha, but correctly consumes wire bytes).
            for (uint32_t row = 0; row < info.h; row++) {
              for (uint32_t col = 0; col < info.w && br.remaining() >= 4; col++) {
                const uint32_t pixel = br.readU32();
                rg.alpha[row * info.w + col] = (uint8_t)((pixel >> 24) & 0xFF);
              }
            }
          } else if (bpp == 8) {
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
              for (uint32_t col = 0; col < info.w; col += 2) {
                if (br.remaining() == 0) break;
                uint8_t byte = br.readU8();
                bytesRead++;
                // Low nibble first (LSBFirst)
                uint8_t lo = byte & 0x0F;
                rg.alpha[row * info.w + col] = (uint8_t)(lo * 255 / 15);
                if (col + 1 < info.w) {
                  uint8_t hi = (byte >> 4) & 0x0F;
                  rg.alpha[row * info.w + col + 1] = (uint8_t)(hi * 255 / 15);
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

#if X11_TRACE_RENDER_ENABLED
    if (nglyphs > 0) {
      const auto& info0 = infos[0];
      fprintf(stderr, "[RENDER] AddGlyphs gsid=0x%x n=%u bpp=%d  "
              "glyph0: id=%u %ux%u origin=(%d,%d) adv=(%d,%d)\n",
              gsid, nglyphs, bpp,
              ids[0], info0.w, info0.h, info0.x, info0.y, info0.xOff, info0.yOff);
    }
#endif
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
    bool srcIsSolid = false;
    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      PictureState* sps = findPicture(srcPid);
      if (sps) {
        if (sps->isSolid) {
          srcIsSolid = true;
          srcColor = sps->solidARGB;
        } else if (sps->repeat && sps->drawable != 0) {
          // Old Xft uses 1×1 Repeat pixmap instead of CreateSolidFill.
          // Promote it to a solid fill by reading the single pixel.
          DrawableRW srcDrw{};
          if (resolveDrawableRW(ctx, sps->drawable, srcDrw) && srcDrw.pixels32) {
            if (srcDrw.w == 1 && srcDrw.h == 1) {
              srcIsSolid = true;
              srcColor = srcDrw.pixels32[0];
#if X11_TRACE_RENDER_ENABLED
              fprintf(stderr, "[RENDER] CompositeGlyphs: promoted 1x1 Repeat to solid=0x%08X\n",
                      srcColor);
#endif
            } else {
              // Larger repeat source — sample first pixel as fallback
              srcIsSolid = true;
              srcColor = srcDrw.pixels32[0];
#if X11_TRACE_RENDER_ENABLED
              fprintf(stderr, "[RENDER] CompositeGlyphs: non-1x1 source %ux%u, using pixel[0]=0x%08X\n",
                      srcDrw.w, srcDrw.h, srcColor);
#endif
            }
          }
        }
      }
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

#if X11_TRACE_RENDER_ENABLED
    fprintf(stderr, "[RENDER] CompositeGlyphs%d op=%d src=0x%x(solid=%d,color=0x%08x) "
            "dst=0x%x(draw=0x%x %ux%u stride=%u) mask=0x%x gs=0x%x\n",
            glyphIdSize * 8, op, srcPid,
            srcIsSolid ? 1 : 0, srcColor,
            dstPid, dstDrawable, dst.w, dst.h, dst.stridePixels,
            maskFmt, gsid);
#endif

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

#if X11_TRACE_RENDER_ENABLED
    if (!cmds.empty()) {
      fprintf(stderr, "[RENDER]   → %zu glyphs, pen=(%d,%d), useMask=%d\n",
              cmds.size(), penX, penY, useMask ? 1 : 0);
      std::lock_guard<std::mutex> lk2(sGlyphMtx);
      for (size_t ci = 0; ci < std::min(cmds.size(), (size_t)3); ci++) {
        auto gsIt = sGlyphSets.find(cmds[ci].gsid);
        if (gsIt == sGlyphSets.end()) continue;
        auto gIt = gsIt->second.glyphs.find(cmds[ci].glyphId);
        if (gIt == gsIt->second.glyphs.end()) continue;
        const auto& rg = gIt->second;
        fprintf(stderr, "[RENDER]   glyph[%zu] id=%u pen=(%d,%d) "
                "%ux%u origin=(%d,%d) → draw@(%d,%d)\n",
                ci, cmds[ci].glyphId, cmds[ci].penX, cmds[ci].penY,
                rg.width, rg.height, rg.x, rg.y,
                cmds[ci].penX - rg.x, cmds[ci].penY - rg.y);
      }
    }
#endif

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

#if X11_TRACE_RENDER_ENABLED
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
#if X11_TRACE_RENDER_ENABLED
    fprintf(stderr, "[RENDER CreateSolidFill] pid=0x%X argb=0x%08X\n",
            pid, ps.solidARGB);
#endif
    return;
  }

  // ---- 34: CreateLinearGradient ----
  case 34: {
    // Wire: pid(4), p1(POINTFIX=8), p2(POINTFIX=8), nStops(4) = 24 header
    //       stops[n](FIXED=4 each), colors[n](COLOR=8 each: r,g,b,a × uint16)
    if (br.remaining() < 24) { br.skip(br.remaining()); return; }
    const uint32_t pid = br.readU32();
    const int32_t p1x  = (int32_t)br.readU32();
    const int32_t p1y  = (int32_t)br.readU32();
    const int32_t p2x  = (int32_t)br.readU32();
    const int32_t p2y  = (int32_t)br.readU32();
    const uint32_t nStops = br.readU32();

    auto gd = std::make_shared<GradientData>();
    gd->type = GradLinear;
    gd->p1x = (float)p1x / 65536.0f;
    gd->p1y = (float)p1y / 65536.0f;
    gd->p2x = (float)p2x / 65536.0f;
    gd->p2y = (float)p2y / 65536.0f;

    // Read stop positions (FIXED 16.16 each)
    std::vector<float> positions;
    positions.reserve(nStops);
    for (uint32_t i = 0; i < nStops && br.remaining() >= 4; i++) {
      int32_t fixed = (int32_t)br.readU32();
      positions.push_back((float)fixed / 65536.0f);
    }
    // Read stop colors (COLOR: r,g,b,a × uint16 = 8 bytes each)
    gd->stops.reserve(nStops);
    for (uint32_t i = 0; i < nStops && br.remaining() >= 8; i++) {
      uint16_t cr = br.readU16();
      uint16_t cg = br.readU16();
      uint16_t cb = br.readU16();
      uint16_t ca = br.readU16();
      GradientStop gs;
      gs.position = (i < positions.size()) ? positions[i] : 0.0f;
      gs.color = renderColorToARGB(cr, cg, cb, ca);
      gd->stops.push_back(gs);
    }
    br.skip(br.remaining());

    PictureState ps;
    ps.format   = kFmtARGB32;
    ps.gradient = gd;
    // If only 1 stop, optimize to solid
    if (gd->stops.size() == 1) {
      ps.isSolid   = true;
      ps.solidARGB = gd->stops[0].color;
      ps.gradient.reset();
    }

    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      sPictures[pid] = std::move(ps);
    }
#ifndef NDEBUG
    fprintf(stderr, "[RENDER CreateLinearGradient] pid=0x%X nStops=%u "
            "p1=(%.1f,%.1f) p2=(%.1f,%.1f)\n",
            pid, nStops, gd->p1x, gd->p1y, gd->p2x, gd->p2y);
#endif
    return;
  }

  // ---- 35: CreateRadialGradient ----
  case 35: {
    // Wire: pid(4), inner_center(POINTFIX=8), outer_center(POINTFIX=8),
    //       inner_radius(FIXED=4), outer_radius(FIXED=4), nStops(4) = 32 header
    if (br.remaining() < 32) { br.skip(br.remaining()); return; }
    const uint32_t pid = br.readU32();
    const int32_t icx  = (int32_t)br.readU32();
    const int32_t icy  = (int32_t)br.readU32();
    const int32_t ocx  = (int32_t)br.readU32();
    const int32_t ocy  = (int32_t)br.readU32();
    const int32_t ir   = (int32_t)br.readU32();
    const int32_t or_  = (int32_t)br.readU32();
    const uint32_t nStops = br.readU32();

    auto gd = std::make_shared<GradientData>();
    gd->type = GradRadial;
    gd->p1x = (float)icx / 65536.0f;
    gd->p1y = (float)icy / 65536.0f;
    gd->p2x = (float)ocx / 65536.0f;
    gd->p2y = (float)ocy / 65536.0f;
    gd->r1  = (float)ir  / 65536.0f;
    gd->r2  = (float)or_ / 65536.0f;

    std::vector<float> positions;
    positions.reserve(nStops);
    for (uint32_t i = 0; i < nStops && br.remaining() >= 4; i++) {
      int32_t fixed = (int32_t)br.readU32();
      positions.push_back((float)fixed / 65536.0f);
    }
    gd->stops.reserve(nStops);
    for (uint32_t i = 0; i < nStops && br.remaining() >= 8; i++) {
      uint16_t cr = br.readU16();
      uint16_t cg = br.readU16();
      uint16_t cb = br.readU16();
      uint16_t ca = br.readU16();
      GradientStop gs;
      gs.position = (i < positions.size()) ? positions[i] : 0.0f;
      gs.color = renderColorToARGB(cr, cg, cb, ca);
      gd->stops.push_back(gs);
    }
    br.skip(br.remaining());

    PictureState ps;
    ps.format   = kFmtARGB32;
    ps.gradient = gd;
    if (gd->stops.size() == 1) {
      ps.isSolid   = true;
      ps.solidARGB = gd->stops[0].color;
      ps.gradient.reset();
    }

    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      sPictures[pid] = std::move(ps);
    }
#ifndef NDEBUG
    fprintf(stderr, "[RENDER CreateRadialGradient] pid=0x%X nStops=%u "
            "inner=(%.1f,%.1f r=%.1f) outer=(%.1f,%.1f r=%.1f)\n",
            pid, nStops, gd->p1x, gd->p1y, gd->r1, gd->p2x, gd->p2y, gd->r2);
#endif
    return;
  }

  // ---- 36: CreateConicalGradient ----
  case 36: {
    // Wire: pid(4), center(POINTFIX=8), angle(FIXED=4), nStops(4) = 20 header
    if (br.remaining() < 20) { br.skip(br.remaining()); return; }
    const uint32_t pid = br.readU32();
    const int32_t cx   = (int32_t)br.readU32();
    const int32_t cy   = (int32_t)br.readU32();
    const int32_t ang  = (int32_t)br.readU32();
    const uint32_t nStops = br.readU32();

    auto gd = std::make_shared<GradientData>();
    gd->type  = GradConical;
    gd->p1x   = (float)cx / 65536.0f;
    gd->p1y   = (float)cy / 65536.0f;
    // RENDER angle is in degrees * 65536 (FIXED 16.16), convert to radians
    gd->angle = ((float)ang / 65536.0f) * (float)M_PI / 180.0f;

    std::vector<float> positions;
    positions.reserve(nStops);
    for (uint32_t i = 0; i < nStops && br.remaining() >= 4; i++) {
      int32_t fixed = (int32_t)br.readU32();
      positions.push_back((float)fixed / 65536.0f);
    }
    gd->stops.reserve(nStops);
    for (uint32_t i = 0; i < nStops && br.remaining() >= 8; i++) {
      uint16_t cr = br.readU16();
      uint16_t cg = br.readU16();
      uint16_t cb = br.readU16();
      uint16_t ca = br.readU16();
      GradientStop gs;
      gs.position = (i < positions.size()) ? positions[i] : 0.0f;
      gs.color = renderColorToARGB(cr, cg, cb, ca);
      gd->stops.push_back(gs);
    }
    br.skip(br.remaining());

    PictureState ps;
    ps.format   = kFmtARGB32;
    ps.gradient = gd;
    if (gd->stops.size() == 1) {
      ps.isSolid   = true;
      ps.solidARGB = gd->stops[0].color;
      ps.gradient.reset();
    }

    {
      std::lock_guard<std::mutex> lk(sPicMtx);
      sPictures[pid] = std::move(ps);
    }
#ifndef NDEBUG
    fprintf(stderr, "[RENDER CreateConicalGradient] pid=0x%X nStops=%u "
            "center=(%.1f,%.1f) angle=%.1f°\n",
            pid, nStops, gd->p1x, gd->p1y, (float)ang / 65536.0f);
#endif
    return;
  }

  default:
    fprintf(stderr, "[RENDER] unhandled minor=%u seq=%u remain=%zu — sending BadRequest\n",
            (unsigned)minor, (unsigned)seq, br.remaining());
    br.skip(br.remaining());
    // Send error to prevent XCB sequence desync if sub-opcode was reply-bearing.
    ctx.transport().sendErrorCore(x11::error::BadRequest, seq, 0, ext::kRENDER);
    return;
  }
}

} // namespace x11
