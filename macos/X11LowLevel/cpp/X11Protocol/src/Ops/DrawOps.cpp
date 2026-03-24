//
//  DrawOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//


#include <cstddef>
#include <cstdint>
#include <cstring>   // memmove
#include <algorithm>


#include "Utils/ByteReader.hpp"
#include "Ops/DrawOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Core/PixmapTable.hpp"   // adjust include path to your project
#include "Core/WindowTable.hpp"
#include "Core/GCTable.hpp"
#include "Core/DrawableRW.hpp"
#include "Ops/DrawOps.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "UI/UICommandQueue.hpp"
#include "Fonts/BDF.hpp"
#include "Fonts/CoreTextFont.hpp"
#include "Core/FontTable.hpp"
#include "Utils/WireEvents.hpp"
#include "Utils/RasterOp.hpp"
#include "Core/SurfaceDesc.hpp"
#include "Core/DrawableSurfaceRegistry.hpp"
#include "Utils/BackgroundFill.hpp"

// bridging
#include "XProtoServerBridge.h"
#include "Core/Font8x8.hpp"
#include "Utils/WireLE.hpp"
#include "Ops/ReplyWriter.hpp"

// util
#include "Damage.hpp"
#include "Utils/GCClip.hpp"
#include "Utils/TraceDefs.hpp"
#include "Utils/MachTime.hpp"

namespace x11 {

static constexpr uint32_t kBitmapScanlinePadBits = 32;   // matches SetupSuccess
static constexpr bool     kBitmapBitOrderLSBFirst = true;

DrawOps::DrawOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::ClearArea, &DrawOps::onMajor, this);  // 61
  reg.registerMajor(x11::opcode::CopyArea,  &DrawOps::onMajor, this);  // 62
  reg.registerMajor(x11::opcode::CopyPlane, &DrawOps::onMajor, this);  // 63
  reg.registerMajor(x11::opcode::PutImage,  &DrawOps::onMajor, this);  // 72
  reg.registerMajor(x11::opcode::GetImage,  &DrawOps::onMajor, this);  // 73
  reg.registerMajor(x11::opcode::PolyText8,   &DrawOps::onMajor, this); // 74
  reg.registerMajor(x11::opcode::PolyText16,  &DrawOps::onMajor, this); // 75
  reg.registerMajor(x11::opcode::ImageText8,  &DrawOps::onMajor, this); // 76
  reg.registerMajor(x11::opcode::ImageText16, &DrawOps::onMajor, this); // 77
}

void DrawOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<DrawOps*>(user)->handle(ctx, dc);
}

void DrawOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::ClearArea: handleClearArea(ctx, dc.seq, dc.minor /*exposures*/, dc.br); return;
    case x11::opcode::CopyArea:  handleCopyArea(ctx, dc.seq, dc.br); return;
    case x11::opcode::CopyPlane: handleCopyPlane(ctx, dc.seq, dc.br); return;
    case x11::opcode::PutImage:  handlePutImage(ctx, dc.seq, dc.minor /*format*/, dc.br); return;
    case x11::opcode::GetImage:  handleGetImage(ctx, dc.seq, dc.minor /*format*/, dc.br); return;
    case x11::opcode::PolyText8:   handlePolyText8(ctx, dc.seq, dc.br); return;  // 74
    case x11::opcode::PolyText16:  handlePolyText16(ctx, dc.seq, dc.br); return; // 75
    case x11::opcode::ImageText8:  handleImageText8(ctx, dc.seq, dc.minor /*n*/, dc.br); return;  // 76
    case x11::opcode::ImageText16: handleImageText16(ctx, dc.seq, dc.minor /*n*/, dc.br); return; // 77
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[DrawOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

uint32_t DrawOps::computeStrideBytesXY1(uint16_t width, uint8_t leftPadBits) {
  const uint32_t bitsPerLine = (uint32_t)leftPadBits + (uint32_t)width;
  const uint32_t paddedBits  = (bitsPerLine + (kBitmapScanlinePadBits - 1u)) & ~(kBitmapScanlinePadBits - 1u);
  return paddedBits / 8u;
}
  
  
// ---------------------------
// helpers
// ---------------------------
  
//static inline void fillRectBGRA(uint32_t* pix, uint32_t fbW, uint32_t fbH,
//                                int x, int y, int w, int h,
//                                uint32_t color)
//{
//  if (!pix || fbW == 0 || fbH == 0) return;
//  if (w <= 0 || h <= 0) return;
//
//  int rx = x, ry = y, rw = w, rh = h;
//
//  if (rx < 0) { rw += rx; rx = 0; }
//  if (ry < 0) { rh += ry; ry = 0; }
//  if (rx + rw > (int)fbW) rw = (int)fbW - rx;
//  if (ry + rh > (int)fbH) rh = (int)fbH - ry;
//  if (rw <= 0 || rh <= 0) return;
//
//  for (int yy = 0; yy < rh; yy++) {
//    uint32_t* row = pix + (size_t)(ry + yy) * fbW + (size_t)rx;
//    for (int xx = 0; xx < rw; xx++) row[xx] = color;
//  }
//}

static inline bool getGC(uint32_t gcXid, x11::GCState& out) {
  return x11::GCTable::instance().find(gcXid, out);
}

static inline const x11::font::BdfFont* resolveFont(x11::XProtoContext& ctx, const x11::GCState& gc) {
  const x11::font::BdfFont* f = nullptr;
  if (gc.font != 0) f = ctx.fonts().get(gc.font);
  if (!f) f = ctx.fonts().findByName("fixed");
  return f;
}

static inline const x11::font::Glyph* resolveGlyph(const x11::font::BdfFont* f, uint8_t ch) {
  if (!f) return nullptr;
  const x11::font::Glyph* g = f->getGlyph((int)ch);
  if (!g) g = f->getGlyph(f->defaultChar);
  if (!g) g = f->getGlyph((int)'?');
  return g;
}
  
  

// -----------------------------
// PutImage (major 72)
// format: dc.minor (1=XYPixmap, 2=ZPixmap)
// We implement the xeyes-critical case first:
//   format=1, depth=1, destination is a depth-1 pixmap (packed bits).
// -----------------------------
void DrawOps::handlePutImage(XProtoContext& ctx, uint16_t seq, uint8_t format, ByteReader& br) {
  // Request body (after 4-byte header):
  //   CARD32 drawable
  //   CARD32 gc
  //   CARD16 width
  //   CARD16 height
  //   INT16  dstX
  //   INT16  dstY
  //   CARD8  leftPad
  //   CARD8  depth
  //   CARD8  pad0
  //   CARD8  pad1
  //   data...
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t drawable = br.readU32();
  const uint32_t gcXid    = br.readU32();

  const uint16_t width  = br.readU16();
  const uint16_t height = br.readU16();
  const int16_t dstX = br.readI16();
  const int16_t dstY = br.readI16();

  const uint8_t leftPad = br.readU8();
  if (leftPad > 7) { br.skip(br.remaining()); return; }

  const uint8_t depth   = br.readU8();
  br.skip(2); // pad0/pad1

  if (width == 0 || height == 0) { br.skip(br.remaining()); return; }

  // Resolve GC for clip + ROP
  x11::GCState piGC{};
  (void)x11::GCTable::instance().find(gcXid, piGC);
  const uint8_t  piFn   = (uint8_t)(piGC.function & 0x0Fu);
  const uint32_t piPm   = piGC.plane_mask;
  const bool     piFast = (piFn == 3) && ((piPm & 0x00FFFFFFu) == 0x00FFFFFFu);

  // Is destination a window?
  const bool dstIsWindow = ctx.windows().exists(drawable);

  // ============================================================================================
  // WINDOW path: ZPixmap depth {24,32} -> resolveDrawableRW (handles child→host offset,
  // border_width, bounds clipping, sibling occlusion)
  // ============================================================================================
  if (dstIsWindow) {
    // Only accept ZPixmap for window surfaces right now.
    // X11 core: format 2 == ZPixmap
    if (format != 2 || (depth != 24 && depth != 32)) {
      br.skip(br.remaining());
      return;
    }

    // Resolve drawable via resolveDrawableRW.  This correctly handles:
    //  - child→host offset (including border_width at each level)
    //  - negative offset clamping
    //  - clipping to child window's own bounds (not just host surface bounds)
    //  - sibling occlusion (edge-based + occluded rects)
    // Previous code walked the parent chain manually but MISSED border_width,
    // causing PutImage content to be misplaced by border_width pixels.
    DrawableRW dst{};
    if (!resolveDrawableRW(ctx, drawable, dst)) {
      br.skip(br.remaining());
      return;
    }
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) {
      br.skip(br.remaining());
      return;
    }

    // For our advertised pixmap formats, depth=24 is still bpp=32 on the wire.
    // Treat both depth=24 and depth=32 as 4 bytes per pixel.
    const uint32_t srcBppBytes = 4u;
    const uint32_t srcStride   = (uint32_t)(((uint32_t)width * srcBppBytes + 3u) & ~3u);

    const uint64_t need64 = (uint64_t)srcStride * (uint64_t)height;
    if (need64 > br.remaining()) {
      br.skip(br.remaining());
      return;
    }

    const uint8_t* src = br.ptr();
    br.skip(br.remaining()); // consume rest (including request padding)

#ifndef NDEBUG
    ctx.tracef("[PutImage/WIN] drawable=0x%08X dst=%d,%d wh=%u,%u depth=%u fmt=%u dstWH=%ux%u stride=%u\n",
               (unsigned)drawable,
               (int)dstX, (int)dstY,
               (unsigned)width, (unsigned)height,
               (unsigned)depth, (unsigned)format,
               (unsigned)dst.w, (unsigned)dst.h, (unsigned)dst.stridePixels);
#endif

    // dstX/dstY are in drawable-local coords.
    // dst.pixels32 is already shifted to the drawable's (0,0) in the host surface.
    // dst.w/dst.h are the drawable's clipped dimensions.
    // Clip the source region to the drawable bounds.
    const int32_t dx0 = (int32_t)dstX;
    const int32_t dy0 = (int32_t)dstY;

    auto copyRegion = [&](int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1) {
      // cx0..cx1, cy0..cy1 are in drawable-local coords, already GC-clipped.
      // Clip to drawable bounds.
      cx0 = std::max<int32_t>(cx0, 0);
      cy0 = std::max<int32_t>(cy0, 0);
      cx1 = std::min<int32_t>(cx1, (int32_t)dst.w);
      cy1 = std::min<int32_t>(cy1, (int32_t)dst.h);
      if (cx0 >= cx1 || cy0 >= cy1) return;

      for (int32_t dy = cy0; dy < cy1; dy++) {
        const int32_t srcRow = dy - dy0;
        if (srcRow < 0 || srcRow >= (int32_t)height) continue;
        const uint8_t* srow = src + (size_t)srcRow * (size_t)srcStride;
        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;

        const int32_t srcX0 = cx0 - dx0;
        const int32_t copyPx = cx1 - cx0;
        if (copyPx <= 0) continue;

        const uint32_t* sp = reinterpret_cast<const uint32_t*>(srow) + srcX0;
        uint32_t* dp = drow + cx0;
        if (piFast) {
          // GXcopy + full planemask: bulk memcpy then force alpha opaque.
          // memcpy is SIMD-optimized by the system library; much faster than
          // per-pixel copy for large spans (e.g., Vivado waveform/schematic).
          std::memcpy(dp, sp, (size_t)copyPx * 4u);
          // Force alpha channel to 0xFF for XRGB8888 surfaces.
          // Process 4 pixels at a time for better throughput.
          int32_t i = 0;
          const int32_t bulk = copyPx & ~3;
          for (; i < bulk; i += 4) {
            dp[i]   |= 0xFF000000u;
            dp[i+1] |= 0xFF000000u;
            dp[i+2] |= 0xFF000000u;
            dp[i+3] |= 0xFF000000u;
          }
          for (; i < copyPx; i++)
            dp[i] |= 0xFF000000u;
        } else {
          for (int32_t i = 0; i < copyPx; i++)
            dp[i] = x11_apply_rop_argb(dp[i], sp[i] | 0xFF000000u, piFn, piPm);
        }
      }
    };

    if (!piGC.has_clip) {
      // No GC clip — copy full region clipped to drawable bounds
      copyRegion(dx0, dy0, dx0 + (int32_t)width, dy0 + (int32_t)height);
    } else {
      // GC clip active — clip in drawable-local coords
      x11::gcClipForEachRect(piGC,
                             dx0, dy0,
                             dx0 + (int32_t)width,
                             dy0 + (int32_t)height,
                             copyRegion);
    }

    // Damage the drawn region (drawable-local coords; damageOrDirty translates to host).
    damageOrDirty(ctx, drawable, dx0, dy0, (int32_t)width, (int32_t)height);
    return;
  }

  // ============================================================================================
  // PIXMAP path: XYBitmap/XYPixmap depth=1
  // ============================================================================================

  // Handle XYBitmap (0) and XYPixmap (1) for depth-1 pixmaps.
  // XYBitmap and XYPixmap carry the same single bitplane for depth=1.
  // XCreateBitmapFromData sends format=0 (XYBitmap), so both must be supported.
  if ((format != 0 && format != 1) || depth != 1) {
    br.skip(br.remaining());
    return;
  }

  // Destination pixmap must be depth=1 (packed bits).
  uint16_t pw = 0, ph = 0;
  uint32_t dstStride = 0;
  uint8_t* dstBits = ctx.pixmaps().mutableBits(drawable, &pw, &ph, &dstStride);
  if (!dstBits || pw == 0 || ph == 0 || dstStride == 0) {
    br.skip(br.remaining());
    return;
  }

  const uint32_t srcStride = computeStrideBytesXY1(width, leftPad);
  const uint64_t need64 = (uint64_t)srcStride * (uint64_t)height;
  if (need64 > br.remaining()) {
    br.skip(br.remaining());
    return;
  }

  const uint8_t* src = br.ptr();   // raw packed bitmap data
  br.skip(br.remaining());         // consume rest (including request padding)

#ifndef NDEBUG
  ctx.tracef("[PutImage/PM] drawable=0x%08X pw=%u ph=%u depth=%u fmt=%u\n",
             (unsigned)drawable, (unsigned)pw, (unsigned)ph, (unsigned)depth, (unsigned)format);
#endif

  // Copy bits from src into dstBits (both LSBFirst as per SetupSuccess).
  // NOTE: PutImage uses leftPad in *source bit indexing*.
  for (uint16_t yy = 0; yy < height; yy++) {
    const int32_t dy = (int32_t)dstY + (int32_t)yy;
    if (dy < 0 || dy >= (int32_t)ph) continue;

    const uint8_t* srow = src + (size_t)yy * (size_t)srcStride;

    for (uint16_t xx = 0; xx < width; xx++) {
      const int32_t dx = (int32_t)dstX + (int32_t)xx;
      if (dx < 0 || dx >= (int32_t)pw) continue;

      // Source bit: leftPad + xx
      const uint32_t bit = (uint32_t)leftPad + (uint32_t)xx;
      const uint32_t sByte = bit >> 3;
      const uint32_t sBit  = bit & 7u;

      const uint8_t sb = srow[sByte];
      const uint8_t smask = kBitmapBitOrderLSBFirst ? (uint8_t)(1u << sBit) : (uint8_t)(1u << (7u - sBit));
      const bool on = (sb & smask) != 0;

      // Destination packed bit at (dx, dy)
      const uint32_t ux = (uint32_t)dx;
      const uint32_t dByte = ux >> 3;
      const uint32_t dBit  = ux & 7u;
      const uint8_t  dmask = kBitmapBitOrderLSBFirst ? (uint8_t)(1u << dBit) : (uint8_t)(1u << (7u - dBit));

      uint8_t* drow = dstBits + (size_t)dy * (size_t)dstStride;
      if (on) drow[dByte] |= dmask;
      else    drow[dByte] &= (uint8_t)~dmask;
    }
  }
}


  
// -----------------------------
// GetImage (major 73)
// format: dc.minor (1=XYPixmap, 2=ZPixmap)
// req: CARD32 drawable, INT16 x, INT16 y, CARD16 w, CARD16 h, CARD32 planeMask
// reply: depth in rep[1], visual in rep[8..11], payload = pixel data
// -----------------------------
void DrawOps::handleGetImage(XProtoContext& ctx, uint16_t seq, uint8_t format, ByteReader& br) {
  if (br.remaining() < 16) { br.skip(br.remaining()); return; }

  const uint32_t drawable = br.readU32();
  const int16_t  x = br.readI16();
  const int16_t  y = br.readI16();
  const uint16_t w = br.readU16();
  const uint16_t h = br.readU16();
  (void)br.readU32(); // planeMask (unused for now)
  br.skip(br.remaining());

  if (w == 0 || h == 0) {
    ctx.transport().sendErrorCore(x11::error::BadValue, seq, 0, x11::opcode::GetImage);
    return;
  }

  // Only support ZPixmap (format 2) for now
  if (format != 2) {
    ctx.transport().sendErrorCore(x11::error::BadValue, seq, (uint32_t)format, x11::opcode::GetImage);
    return;
  }

  // Resolve drawable
  x11::DrawableRW src{};
  if (!x11::resolveDrawableRW(ctx, drawable, src) || !src.pixels32) {
    ctx.transport().sendErrorCore(x11::error::BadDrawable, seq, drawable, x11::opcode::GetImage);
    return;
  }
  if (src.w == 0 || src.h == 0 || src.stridePixels == 0) {
    ctx.transport().sendErrorCore(x11::error::BadDrawable, seq, drawable, x11::opcode::GetImage);
    return;
  }

  // Clip request rect to drawable bounds
  int32_t x0 = (int32_t)x;
  int32_t y0 = (int32_t)y;
  int32_t x1 = x0 + (int32_t)w;
  int32_t y1 = y0 + (int32_t)h;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > (int32_t)src.w) x1 = (int32_t)src.w;
  if (y1 > (int32_t)src.h) y1 = (int32_t)src.h;

  const int32_t cw = x1 - x0;
  const int32_t ch = y1 - y0;
  if (cw <= 0 || ch <= 0) return;

  // Build pixel payload: ZPixmap depth=24 bpp=32
  // Each row: cw * 4 bytes, padded to 4-byte boundary (already aligned since 4*cw is always multiple of 4)
  const uint32_t rowBytes = (uint32_t)cw * 4u;
  const uint32_t payloadBytes = rowBytes * (uint32_t)ch;
  const uint32_t payloadWords = (payloadBytes + 3u) / 4u;

  // Build reply header
  const uint8_t depth = 24;
  const uint32_t visual = 0x21; // match our advertised TrueColor visual

  const bool ok = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = depth;
    wire::wr32_le(rep.data() + 4, payloadWords);
    wire::wr32_le(rep.data() + 8, visual);
  });
  if (!ok) return;

  // Send pixel rows
  for (int32_t row = 0; row < ch; row++) {
    const uint32_t* srcRow = src.pixels32 + (size_t)(y0 + row) * (size_t)src.stridePixels + (size_t)x0;
    if (!ctx.reply().sendBytes(srcRow, rowBytes)) return;
  }

  ctx.tracef("[GetImage] drawable=0x%08X x=%d y=%d w=%u h=%u -> %ux%u depth=%u\n",
             (unsigned)drawable, (int)x, (int)y, (unsigned)w, (unsigned)h,
             (unsigned)cw, (unsigned)ch, (unsigned)depth);
}

// -----------------------------
// CopyArea (major 62)
// -----------------------------
void DrawOps::handleCopyArea(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 24) { br.skip(br.remaining()); return; }

  const uint32_t src   = br.readU32();
  const uint32_t dst   = br.readU32();
  const uint32_t gcXid = br.readU32();

  int32_t srcX = br.readI16();
  int32_t srcY = br.readI16();
  int32_t dstX = br.readI16();
  int32_t dstY = br.readI16();

  int32_t wpx = (int32_t)br.readU16();
  int32_t hpx = (int32_t)br.readU16();
  br.skip(br.remaining());

  if (wpx <= 0 || hpx <= 0) return;

  // ------------------------------------------------------------
  // Resolve GC (function + plane mask)
  // ------------------------------------------------------------
  x11::GCState gc{};
  if (!x11::GCTable::instance().find(gcXid, gc)) {
    gc = x11::GCTable::instance().getOrCreate(gcXid);
  }

  const uint8_t  fn   = gc.function;
  const uint32_t pm24 = (gc.plane_mask & 0x00FFFFFFu);

  const bool isGXcopy = ((fn & 0x0Fu) == 3); // GXcopy == 3
  const bool fullPlane = (pm24 == 0x00FFFFFFu);
  const bool canMemmoveFast = (isGXcopy && fullPlane);

  // ------------------------------------------------------------
  // Resolve source drawable -> read pointer + dims + stride + backing info
  // ------------------------------------------------------------
  x11::DrawableRW srcRW{};
  const uint32_t* srcPixels = nullptr;
  int srcW = 0, srcH = 0;
  uint32_t srcStride = 0;

  const bool srcIsWin = ctx.windows().exists(src);
  const bool srcIsPix = ctx.pixmaps().exists(src);

  if (srcIsWin) {
    if (!x11::resolveDrawableRW(ctx, src, srcRW) || !srcRW.pixels32) return;
    srcPixels = srcRW.pixels32;
    srcW = (int)srcRW.w;
    srcH = (int)srcRW.h;
    srcStride = srcRW.stridePixels;
    if (srcW <= 0 || srcH <= 0 || srcStride == 0) return;
  } else if (srcIsPix) {
    PixmapView pv{};
    if (!ctx.pixmaps().snapshot(src, pv)) return;
    if (pv.depth == 1) return; // CopyArea not for depth-1 masks
    if (!pv.pixels || pv.w == 0 || pv.h == 0) return;

    srcPixels = pv.pixels;
    srcW = (int)pv.w;
    srcH = (int)pv.h;
    srcStride = (uint32_t)pv.w;

    srcRW.isPixmap = true;
    srcRW.isWindow = false;
    srcRW.pixels32 = const_cast<uint32_t*>(pv.pixels); // backing identity only
    srcRW.w = pv.w;
    srcRW.h = pv.h;
    srcRW.stridePixels = srcStride;
    srcRW.backingPixels32 = const_cast<uint32_t*>(pv.pixels);
    srcRW.backingStridePixels = srcStride;
    srcRW.backingXid = src;
    srcRW.offsetX = 0;
    srcRW.offsetY = 0;
  } else {
    ctx.transport().sendErrorCore(x11::error::BadDrawable, seq, src, x11::opcode::CopyArea);
    return;
  }

  // ------------------------------------------------------------
  // Resolve destination drawable -> write pointer + dims + stride + backing info
  // ------------------------------------------------------------
  x11::DrawableRW dstRW{};
  uint32_t* dstPixels = nullptr;
  int dstW = 0, dstH = 0;
  uint32_t dstStride = 0;

  const bool dstIsWin = ctx.windows().exists(dst);
  const bool dstIsPix = ctx.pixmaps().exists(dst);

  if (dstIsWin) {
    if (!x11::resolveDrawableRW(ctx, dst, dstRW) || !dstRW.pixels32) return;
    dstPixels = dstRW.pixels32;
    dstW = (int)dstRW.w;
    dstH = (int)dstRW.h;
    dstStride = dstRW.stridePixels;
    if (dstW <= 0 || dstH <= 0 || dstStride == 0) return;
  } else if (dstIsPix) {
    uint16_t pw = 0, ph = 0;
    dstPixels = ctx.pixmaps().mutablePixels(dst, &pw, &ph);
    if (!dstPixels || pw == 0 || ph == 0) return;

    dstW = (int)pw;
    dstH = (int)ph;
    dstStride = (uint32_t)pw;

    dstRW.isPixmap = true;
    dstRW.isWindow = false;
    dstRW.pixels32 = dstPixels;
    dstRW.w = pw;
    dstRW.h = ph;
    dstRW.stridePixels = dstStride;
    dstRW.backingPixels32 = dstPixels;
    dstRW.backingStridePixels = dstStride;
    dstRW.backingXid = dst;
    dstRW.offsetX = 0;
    dstRW.offsetY = 0;
  } else {
    ctx.transport().sendErrorCore(x11::error::BadDrawable, seq, dst, x11::opcode::CopyArea);
    return;
  }

  // Same underlying buffer? (important now that child windows share host backing)
  const bool sameBacking =
    (srcRW.backingPixels32 != nullptr) &&
    (srcRW.backingPixels32 == dstRW.backingPixels32) &&
    (srcRW.backingStridePixels == dstRW.backingStridePixels);

#ifdef X11_TRACE_VERBOSE
  if (sameBacking || (wpx * hpx) > 4096) {
    fprintf(stderr,
            "[CopyArea] src=0x%08X dst=0x%08X sameBacking=%d "
            "srcXY=(%d,%d) dstXY=(%d,%d) wh=%dx%d srcWH=%dx%d dstWH=%dx%d fn=%u pm=0x%08X\n",
            (unsigned)src, (unsigned)dst, sameBacking ? 1 : 0,
            (int)srcX, (int)srcY, (int)dstX, (int)dstY,
            (int)wpx, (int)hpx,
            srcW, srcH, dstW, dstH,
            (unsigned)fn, (unsigned)gc.plane_mask);
  }
#endif

  // ------------------------------------------------------------
  // Clip / clamp copy rectangle (adjusting both sides)
  // ------------------------------------------------------------
  int sx0 = (int)srcX;
  int sy0 = (int)srcY;
  int dx0 = (int)dstX;
  int dy0 = (int)dstY;
  int cw  = (int)wpx;
  int ch  = (int)hpx;

  if (sx0 < 0) { int d = -sx0; sx0 = 0; dx0 += d; cw -= d; }
  if (sy0 < 0) { int d = -sy0; sy0 = 0; dy0 += d; ch -= d; }
  if (dx0 < 0) { int d = -dx0; dx0 = 0; sx0 += d; cw -= d; }
  if (dy0 < 0) { int d = -dy0; dy0 = 0; sy0 += d; ch -= d; }

  if (sx0 + cw > srcW) cw = srcW - sx0;
  if (dx0 + cw > dstW) cw = dstW - dx0;
  if (sy0 + ch > srcH) ch = srcH - sy0;
  if (dy0 + ch > dstH) ch = dstH - dy0;

  if (cw <= 0 || ch <= 0) return;

  // ------------------------------------------------------------
  // Overlap detection in backing coordinates (host coords)
  // ------------------------------------------------------------
  const int srcAbsX0 = srcRW.offsetX + sx0;
  const int srcAbsY0 = srcRW.offsetY + sy0;
  const int dstAbsX0 = dstRW.offsetX + dx0;
  const int dstAbsY0 = dstRW.offsetY + dy0;

  const bool overlaps =
    sameBacking &&
    (dstAbsX0 < srcAbsX0 + cw) && (dstAbsX0 + cw > srcAbsX0) &&
    (dstAbsY0 < srcAbsY0 + ch) && (dstAbsY0 + ch > srcAbsY0);

  // ------------------------------------------------------------
  // Copy / ROP with overlap-safe ordering + GC clip
  // ------------------------------------------------------------
  const bool gcClipActive = gc.has_clip;

  auto rowCopyFast = [&](int sy, int dy) {
    const uint32_t* sp = srcPixels + (size_t)sy * (size_t)srcStride + (size_t)sx0;
    uint32_t*       dp = dstPixels + (size_t)dy * (size_t)dstStride + (size_t)dx0;
    if (!gcClipActive) {
      std::memmove(dp, sp, (size_t)cw * sizeof(uint32_t));
      for (int i = 0; i < cw; i++) dp[i] = (dp[i] & 0x00FFFFFFu) | 0xFF000000u;
    } else {
      for (int i = 0; i < cw; i++) {
        if (!x11::gcPointVisible(gc, dx0 + i, dy)) continue;
        dp[i] = (sp[i] & 0x00FFFFFFu) | 0xFF000000u;
      }
    }
  };

  auto rowRop = [&](int sy, int dy, bool rightToLeft) {
    const uint32_t* sp = srcPixels + (size_t)sy * (size_t)srcStride + (size_t)sx0;
    uint32_t*       dp = dstPixels + (size_t)dy * (size_t)dstStride + (size_t)dx0;

    if (!rightToLeft) {
      for (int i = 0; i < cw; i++) {
        if (gcClipActive && !x11::gcPointVisible(gc, dx0 + i, dy)) continue;
        uint32_t out = x11_apply_rop_argb(dp[i], sp[i], fn, gc.plane_mask);
        dp[i] = (out & 0x00FFFFFFu) | 0xFF000000u;
      }
    } else {
      for (int i = cw - 1; i >= 0; i--) {
        if (gcClipActive && !x11::gcPointVisible(gc, dx0 + i, dy)) continue;
        uint32_t out = x11_apply_rop_argb(dp[i], sp[i], fn, gc.plane_mask);
        dp[i] = (out & 0x00FFFFFFu) | 0xFF000000u;
      }
    }
  };

  int rowStart = 0, rowEnd = ch, rowStep = 1;
  if (overlaps && dstAbsY0 > srcAbsY0) {
    rowStart = ch - 1;
    rowEnd   = -1;
    rowStep  = -1;
  }

  for (int r = rowStart; r != rowEnd; r += rowStep) {
    const int sy = sy0 + r;
    const int dy = dy0 + r;

    if (canMemmoveFast && !gcClipActive) {
      rowCopyFast(sy, dy);
    } else if (canMemmoveFast) {
      rowCopyFast(sy, dy); // handles clip inside
    } else {
      bool rtl = false;
      if (overlaps && (dstAbsY0 + r) == (srcAbsY0 + r)) {
        rtl = (dstAbsX0 > srcAbsX0);
      }
      rowRop(sy, dy, rtl);
    }
  }

  // ------------------------------------------------------------
  // Damage only if destination is a window
  // ------------------------------------------------------------
  if (dstIsWin) {
    damageOrDirty(ctx, dst, (int32_t)dx0, (int32_t)dy0, (int32_t)cw, (int32_t)ch);
  }

  // X11 spec: NoExposure sent when graphics_exposures is True in the GC
  // and no region of the source is obscured.
  if (gc.graphics_exposures) {
    auto ev = x11::wireev::buildNoExpose(seq, dst,
                                        x11::opcode::CopyArea, 0);
    (void)ctx.transport().sendEvent32(dst, ev.data());
  }
}
  
// -----------------------------
// CopyPlane (major 63)
// -----------------------------
  void DrawOps::handleCopyPlane(XProtoContext& ctx, uint16_t seq, ByteReader& br)
  {
    // Body after 4-byte header (28 bytes):
    //   CARD32 srcDrawable
    //   CARD32 dstDrawable
    //   CARD32 gc
    //   INT16  srcX
    //   INT16  srcY
    //   INT16  dstX
    //   INT16  dstY
    //   CARD16 width
    //   CARD16 height
    //   CARD32 bitPlane
    if (br.remaining() < 28) { br.skip(br.remaining()); return; }
    
    const uint32_t src = br.readU32();
    const uint32_t dst = br.readU32();
    const uint32_t gc  = br.readU32();
    
    const int16_t srcX = br.readI16();
    const int16_t srcY = br.readI16();
    const int16_t dstX = br.readI16();
    const int16_t dstY = br.readI16();
    
    const uint16_t wpx = br.readU16();
    const uint16_t hpx = br.readU16();
    
    const uint32_t bitPlane = br.readU32();
    br.skip(br.remaining());
    
    if (wpx == 0 || hpx == 0) return;
    
    // For depth-1 sources the only meaningful plane is 1.
    // Accept any single-bit plane to be permissive, but behavior is identical.
    if ((bitPlane == 0) || (bitPlane & (bitPlane - 1)) != 0) return; // must be power of two
    
    // We advertise bitmapBitOrder = LSBFirst in SetupSuccess.
    const bool BIT_ORDER_LSB_FIRST = true;
    
    // ------------------------------------------------------------
    // Resolve GC fg/bg
    // ------------------------------------------------------------
    uint32_t fg = 0xFF000000u;
    uint32_t bg = 0xFFFFFFFFu;
    
    GCState gst{};
    if (GCTable::instance().find(gc, gst)) {
      fg = gst.fg;
      bg = gst.bg;
    }
    const uint8_t  cpFn   = (uint8_t)(gst.function & 0x0Fu);
    const uint32_t cpPm   = gst.plane_mask;
    const bool     cpFast = (cpFn == 3) && ((cpPm & 0x00FFFFFFu) == 0x00FFFFFFu);
    
    // ------------------------------------------------------------
    // Resolve src as either:
    //  - depth-1 pixmap bits, OR
    //  - 32bpp pixels (window FB or depth>1 pixmap)
    // ------------------------------------------------------------
    const uint32_t* srcPixels = nullptr;
    const uint8_t*  srcBits   = nullptr;
    int srcW = 0, srcH = 0;
    uint32_t srcStrideBytes = 0;
    bool srcDepth1 = false;
    
    const bool srcIsWin = ctx.windows().exists(src);
    const bool srcIsPix = ctx.pixmaps().exists(src);
    
    if (srcIsWin) {
      DrawableRW srcRW{};
      if (!resolveDrawableRW(ctx, src, srcRW) || !srcRW.pixels32) return;
      srcPixels = srcRW.pixels32;
      srcW = (int)srcRW.w;
      srcH = (int)srcRW.h;
      srcDepth1 = false;
    } else if (srcIsPix) {
      PixmapView pv{};
      if (!ctx.pixmaps().snapshot(src, pv)) return;
      srcW = (int)pv.w;
      srcH = (int)pv.h;
      
      if (pv.depth == 1) {
        if (!pv.bits || pv.stride_bytes == 0) return;
        srcBits = pv.bits;
        srcStrideBytes = pv.stride_bytes;
        srcDepth1 = true;
      } else {
        if (!pv.pixels) return;
        srcPixels = pv.pixels;
        srcDepth1 = false;
      }
    } else {
      ctx.transport().sendErrorCore(x11::error::BadDrawable, seq, src, x11::opcode::CopyPlane);
      return;
    }

    // Bring-up correctness: CopyPlane is expected from depth-1 pixmaps.
    // If the source isn't depth-1, do nothing (better than wrong masks).
    if (!srcDepth1) return;
    
    
    // ------------------------------------------------------------
    // Resolve dst as either:
    //  - depth-1 pixmap bits, OR
    //  - 32bpp pixels (window Swift surface or depth>1 pixmap)
    // ------------------------------------------------------------
    uint32_t* dstPixels = nullptr;
    uint8_t*  dstBits   = nullptr;
    int dstW = 0, dstH = 0;
    uint32_t dstStrideBytes = 0;
    uint32_t dstStridePx = 0;  // stride in pixels for 32bpp destinations
    bool dstDepth1 = false;
    bool dstIsWindow = false;

    if (ctx.windows().exists(dst)) {
      // Use resolveDrawableRW so CopyPlane writes to the Swift-owned surface,
      // not the old C framebuffer.  This mirrors every other drawing op.
      x11::DrawableRW dstRW{};
      if (!x11::resolveDrawableRW(ctx, dst, dstRW) || !dstRW.pixels32) return;
      dstPixels   = dstRW.pixels32;
      dstW        = (int)dstRW.w;
      dstH        = (int)dstRW.h;
      dstStridePx = dstRW.stridePixels;
      dstDepth1   = false;
      dstIsWindow = true;
    } else {
      // try depth-1 bits first
      uint16_t pw = 0, ph = 0;
      uint32_t stride = 0;
      if (uint8_t* bits = ctx.pixmaps().mutableBits(dst, &pw, &ph, &stride)) {
        dstBits        = bits;
        dstW           = (int)pw;
        dstH           = (int)ph;
        dstStrideBytes = stride;
        dstStridePx    = (uint32_t)pw;   // packed bits; stride in px == width
        dstDepth1      = true;
        dstIsWindow    = false;
      } else {
        uint16_t pw2 = 0, ph2 = 0;
        uint32_t* pix = ctx.pixmaps().mutablePixels(dst, &pw2, &ph2);
        if (!pix) return;
        dstPixels   = pix;
        dstW        = (int)pw2;
        dstH        = (int)ph2;
        dstStridePx = (uint32_t)pw2;   // pixmaps are tight
        dstDepth1   = false;
        dstIsWindow = false;
      }
    }
    
    // ------------------------------------------------------------
    // Raster loop: interpret src as 1-bit plane and write to dst:
    //  - if dstDepth1: write packed bit
    //  - else: write fg/bg pixels
    // ------------------------------------------------------------
    for (int yy = 0; yy < (int)hpx; yy++) {
      const int sy = (int)srcY + yy;
      const int dy = (int)dstY + yy;
      if ((unsigned)sy >= (unsigned)srcH) continue;
      if ((unsigned)dy >= (unsigned)dstH) continue;
      
      for (int xx = 0; xx < (int)wpx; xx++) {
        const int sx = (int)srcX + xx;
        const int dx = (int)dstX + xx;
        if ((unsigned)sx >= (unsigned)srcW) continue;
        if ((unsigned)dx >= (unsigned)dstW) continue;
        
        int on = 0;
        
        if (srcDepth1) {
          const size_t byteIndex = (size_t)sy * (size_t)srcStrideBytes + ((size_t)sx >> 3);
          const int bitInByte = BIT_ORDER_LSB_FIRST ? (sx & 7) : (7 - (sx & 7));
          on = (srcBits[byteIndex] >> bitInByte) & 1;
        } else {
          // unreachable because we return above if !srcDepth1
          on = 0;
        }
        
        if (dstDepth1) {
          const size_t dByteIndex = (size_t)dy * (size_t)dstStrideBytes + ((size_t)dx >> 3);
          const int dBitInByte = BIT_ORDER_LSB_FIRST ? (dx & 7) : (7 - (dx & 7));
          const uint8_t mask = (uint8_t)(1u << dBitInByte);
          if (on) dstBits[dByteIndex] |= mask;
          else    dstBits[dByteIndex] &= (uint8_t)~mask;
        } else {
          if (gst.has_clip && !x11::gcPointVisible(gst, dx, dy)) continue;
          const uint32_t src_px = on ? fg : bg;
          uint32_t& d = dstPixels[(size_t)dy * (size_t)dstStridePx + (size_t)dx];
          if (cpFast) d = src_px;
          else        d = x11_apply_rop_argb(d, src_px, cpFn, cpPm);
        }
      }
    }
    
    // Damage only if destination is a window.
    if ( dstIsWindow ) {
      damageOrDirty(ctx, dst, (int32_t)dstX, (int32_t)dstY, (int32_t)wpx, (int32_t)hpx);
    }

    // X11 spec: NoExposure sent when graphics_exposures is True in the GC
    if (gst.graphics_exposures) {
      auto noExpEv = x11::wireev::buildNoExpose(seq, dst,
                                                x11::opcode::CopyPlane, 0);
      (void)ctx.transport().sendEvent32(dst, noExpEv.data());
    }
  }


  void DrawOps::handleClearArea(XProtoContext& ctx, uint16_t seq, uint8_t exposures, ByteReader& br)
  {
    if (br.remaining() < 12) { br.skip(br.remaining()); return; }

    const uint32_t wid = br.readU32();
    const int16_t  x   = br.readI16();
    const int16_t  y   = br.readI16();
    const uint16_t wpx = br.readU16();
    const uint16_t hpx = br.readU16();
    br.skip(br.remaining());

    ctx.tracef("[ClearArea] wid=0x%08X xy=(%d,%d) wh=(%u,%u) exp=%u\n",
              wid, (int)x, (int)y, (unsigned)wpx, (unsigned)hpx,
              (unsigned)exposures);

    if (wid == 0) return;
    if (!ctx.windows().exists(wid)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::ClearArea);
      return;
    }

    // Resolve writable pixels (SurfaceRegistry preferred; C FB fallback).
    x11::DrawableRW dst{};
    if (!x11::resolveDrawableRW(ctx, wid, dst)) return;
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) return;

    int x0 = (int)x;
    int y0 = (int)y;

    // clamp start
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 > (int)dst.w) x0 = (int)dst.w;
    if (y0 > (int)dst.h) y0 = (int)dst.h;

    // width/height == 0 => to edge from clamped origin
    int x1 = (wpx == 0) ? (int)dst.w : (x0 + (int)wpx);
    int y1 = (hpx == 0) ? (int)dst.h : (y0 + (int)hpx);

    // clamp end
    if (x1 > (int)dst.w) x1 = (int)dst.w;
    if (y1 > (int)dst.h) y1 = (int)dst.h;

    if (x0 >= x1 || y0 >= y1) return;

    // X11 spec: ClearArea clears to the window's background.
    // Check for background pixmap first, then solid pixel.
    // If no background defined (CWBackPixmap=None), do NOT modify contents.
    bool filled = false;

    uint32_t bgPixmap = 0;
    if (ctx.windows().resolveBackgroundPixmapForClear(wid, bgPixmap)) {
      // Tiled pixmap background
      filled = x11::tilePixmapFill(ctx, bgPixmap, dst, x0, y0, x1, y1);
    }

    if (!filled) {
      uint32_t bg = 0;
      if (!ctx.windows().resolveBackgroundForClear(wid, bg)) {
        // No background — per X11 spec, ClearArea has no effect on contents.
        // Still send Expose if requested.
        if (exposures && dst.isWindow) {
          ctx.transport().queueExposeRect(
            wid,
            (uint16_t)x0, (uint16_t)y0,
            (uint16_t)(x1 - x0), (uint16_t)(y1 - y0),
            0
          );
        }
        return;
      }

#if X11_TRACE_FONT_ENABLED
      TS_FPRINTF("[CA_DBG] wid=0x%X clear=[%d,%d]->[%d,%d] (%dx%d) dst=%ux%u\n",
              (unsigned)wid, x0, y0, x1, y1,
              x1 - x0, y1 - y0, (unsigned)dst.w, (unsigned)dst.h);
#endif

      if (dst.numOccluded > 0) {
        for (int yy = y0; yy < y1; yy++) {
          uint32_t* row = dst.pixels32 + (size_t)yy * (size_t)dst.stridePixels;
          for (int xx = x0; xx < x1; xx++) {
            if (!dst.isOccluded(xx, yy)) row[(size_t)xx] = bg;
          }
        }
      } else {
        for (int yy = y0; yy < y1; yy++) {
          uint32_t* row = dst.pixels32 + (size_t)yy * (size_t)dst.stridePixels;
          for (int xx = x0; xx < x1; xx++) row[(size_t)xx] = bg;
        }
      }
    }

    // Damage -> present (only meaningful for windows)
    if (dst.isWindow) {
      damageOrDirty(ctx, wid, (int32_t)x0, (int32_t)y0, (int32_t)(x1 - x0), (int32_t)(y1 - y0));
    }

    // Exposures requested? Queue expose rect (Transport flush will filter by mask/mapped).
    if (exposures && dst.isWindow) {
      ctx.transport().queueExposeRect(
        wid,
        (uint16_t)x0, (uint16_t)y0,
        (uint16_t)(x1 - x0), (uint16_t)(y1 - y0),
        0
      );
    }
  }
  
  
  // -----------------------------
  // PolyText8 (major 74)
  // -----------------------------
  void DrawOps::handlePolyText8(XProtoContext& ctx, uint16_t seq, ByteReader& br)
  {
    // Body:
    //   CARD32 drawable
    //   CARD32 gc
    //   INT16 x
    //   INT16 y
    //   LISTofTEXTITEM8 items...
    //
    // TEXTITEM8:
    //   BYTE len;          // 0..254 => string item, 255 => font change
    //   INT8 delta;        // signed
    //   if len == 255: CARD32 font
    //   else: CHAR string[len]

    if (br.remaining() < 12) { br.skip(br.remaining()); return; }

    const uint32_t drawable = br.readU32();
    const uint32_t gcXid    = br.readU32();
    int32_t penX            = (int16_t)br.readU16();
    const int32_t baseY     = (int16_t)br.readU16();

    // Verbose OR_TEXT trace removed (was diagnostic-only for v1.10.4–v1.10.7).
    // The trace checked every PolyText8 on OR windows for surface resolution status.
#if X11_TRACE_FONT_ENABLED
    {
      x11::WindowView wv{};
      bool isWin = ctx.windows().snapshot(drawable, wv);
      if (isWin) {
        TS_FPRINTF("[LABEL] PolyText8 drawable=0x%08X parent=0x%08X pos=(%d,%d) size=%ux%u at (%d,%d)\n",
                (unsigned)drawable, (unsigned)wv.parent_xid,
                (int)wv.x, (int)wv.y, (unsigned)wv.w, (unsigned)wv.h,
                (int)penX, (int)baseY);
      }
    }
#endif

    x11::DrawableRW dst{};
    if (!x11::resolveDrawableRW(ctx, drawable, dst)) {
      br.skip(br.remaining());
      if (!ctx.windows().exists(drawable) && !ctx.pixmaps().exists(drawable))
        ctx.transport().sendErrorCore(x11::error::BadDrawable, seq, drawable, x11::opcode::PolyText8);
      return;
    }
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) { br.skip(br.remaining()); return; }

    // Get GC
    x11::GCState gc{};
    (void)getGC(gcXid, gc);

    // Resolve font from GC
    const x11::font::BdfFont* f = resolveFont(ctx, gc);
    if (!f) { br.skip(br.remaining()); return; }

    const x11::GCState* gcClip = gc.has_clip ? &gc : nullptr;
    const uint8_t ptFn = (uint8_t)(gc.function & 0x0Fu);
    const uint32_t ptPm = gc.plane_mask;
    const bool ptFast = (ptFn == 3) && ((ptPm & 0x00FFFFFFu) == 0x00FFFFFFu);

    auto drawGlyph1bpp32 = [&](int leftX, int topY, const x11::font::Glyph& g, uint32_t fg) {
      const int gw = g.bbx_w;
      const int gh = g.bbx_h;
      if (gw <= 0 || gh <= 0) return;

      const uint32_t fgO = (fg & 0x00FFFFFFu) | 0xFF000000u;

      const int srcStride = g.rowStrideBytes();
      if (srcStride <= 0) return;
      const size_t need = (size_t)srcStride * (size_t)gh;
      if (g.bitmap.size() < need) return;

      for (int yy = 0; yy < gh; yy++) {
        const int dy = topY + yy;
        if (dy < 0 || dy >= (int)dst.h) continue;

        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        const uint8_t* srow = g.bitmap.data() + (size_t)yy * (size_t)srcStride;

        for (int xx = 0; xx < gw; xx++) {
          const int dx = leftX + xx;
          if (dx < 0 || dx >= (int)dst.w) continue;

          const uint32_t bit = (uint32_t)xx;
          const uint32_t sByte = bit >> 3;
          const uint32_t sBit  = bit & 7u;

          // MSBFirst (fixes mirrored glyphs)
          const uint8_t mask = (uint8_t)(1u << (7u - sBit));
          const bool on = (srow[sByte] & mask) != 0;

          if (on) {
            if (gcClip && !x11::gcPointVisible(*gcClip, dx, dy)) continue;
            if (ptFast) drow[(size_t)dx] = fgO;
            else        drow[(size_t)dx] = x11_apply_rop_argb(drow[(size_t)dx], fgO, ptFn, ptPm);
          }
        }
      }
    };

    // Alpha-blended glyph rendering for antialiased CoreText fonts
    auto drawGlyphAlpha32 = [&](int leftX, int topY, const x11::font::Glyph& g, uint32_t fg) {
      const int gw = g.bbx_w, gh = g.bbx_h;
      if (gw <= 0 || gh <= 0) return;
      if ((int)g.alpha.size() < gw * gh) return;

      const uint8_t fgR = (uint8_t)((fg >> 16) & 0xFFu);
      const uint8_t fgG = (uint8_t)((fg >>  8) & 0xFFu);
      const uint8_t fgB = (uint8_t)( fg        & 0xFFu);

      for (int yy = 0; yy < gh; yy++) {
        const int dy = topY + yy;
        if (dy < 0 || dy >= (int)dst.h) continue;
        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        const uint8_t* arow = g.alpha.data() + (size_t)yy * (size_t)gw;
        for (int xx = 0; xx < gw; xx++) {
          const int dx = leftX + xx;
          if (dx < 0 || dx >= (int)dst.w) continue;
          const uint8_t a = arow[xx];
          if (a == 0) continue;
          if (gcClip && !x11::gcPointVisible(*gcClip, dx, dy)) continue;
          if (a == 255) {
            drow[(size_t)dx] = (fg & 0x00FFFFFFu) | 0xFF000000u;
          } else {
            const uint32_t bg = drow[(size_t)dx];
            const uint8_t bgR = (uint8_t)((bg >> 16) & 0xFFu);
            const uint8_t bgG = (uint8_t)((bg >>  8) & 0xFFu);
            const uint8_t bgB = (uint8_t)( bg        & 0xFFu);
            const uint8_t oR = (uint8_t)((fgR * a + bgR * (255u - a) + 127u) / 255u);
            const uint8_t oG = (uint8_t)((fgG * a + bgG * (255u - a) + 127u) / 255u);
            const uint8_t oB = (uint8_t)((fgB * a + bgB * (255u - a) + 127u) / 255u);
            drow[(size_t)dx] = 0xFF000000u | ((uint32_t)oR << 16) | ((uint32_t)oG << 8) | (uint32_t)oB;
          }
        }
      }
    };

    const bool useAA = x11::font::antialiasedFonts();

#if X11_TRACE_FONT_ENABLED
    TS_FPRINTF("[PT8_DBG] draw=0x%X pen=(%d,%d) fontAsc=%d fontDesc=%d "
            "dst=%ux%u font=\"%s\"\n",
            (unsigned)drawable, (int)penX, (int)baseY,
            f->ascent, f->descent,
            (unsigned)dst.w, (unsigned)dst.h,
            f->name.c_str());
#endif

    // Parse items until end of request body.
    while (br.remaining() >= 2) {
      const uint8_t len = br.readU8();
      const int8_t  delta = (int8_t)br.readU8();

      penX += (int32_t)delta;

      if (len == 0) continue;

      if (len == 255) {
        // Font change item: CARD32 font
        if (br.remaining() < 4) { br.skip(br.remaining()); break; }
        const uint32_t newFont = br.readU32();
        gc.font = newFont;
        const x11::font::BdfFont* nf = resolveFont(ctx, gc);
        if (nf) f = nf;
        continue;
      }

      if (br.remaining() < len) { br.skip(br.remaining()); break; }

#if X11_TRACE_FONT_ENABLED
      {
        const uint8_t* peek = br.peekBytes(len);
        if (peek) {
          std::string seg((const char*)peek, len);
          TS_FPRINTF("[LABEL]   PolyText8 item drawable=0x%08X text=\"%s\"\n",
                  (unsigned)drawable, seg.c_str());
        }
      }
#endif

      for (uint8_t i = 0; i < len; i++) {
        const uint8_t ch = br.readU8();

        const x11::font::Glyph* g = resolveGlyph(f, ch);
        if (!g) { penX += f->advanceFor((int)ch); continue; }

        const int leftX = (int)penX + g->bbx_xoff;
        const int topY  = (int)baseY - g->bbx_yoff - g->bbx_h;

        if (g->hasAlpha() && useAA)
          drawGlyphAlpha32(leftX, topY, *g, gc.fg);
        else
          drawGlyph1bpp32(leftX, topY, *g, gc.fg);

        penX += (g->dwidth != 0) ? g->dwidth : f->advanceFor((int)ch);
      }
    }

    br.skip(br.remaining());

    if (dst.isWindow) {
      // PolyText8: computing exact glyph bounding box is complex; use full drawable.
      damageOrDirty(ctx, drawable, 0, 0, (int32_t)dst.w, (int32_t)dst.h);
    }
  }


  // -----------------------------
  // ImageText8 (major 76)
  // -----------------------------
  void DrawOps::handleImageText8(XProtoContext& ctx, uint16_t seq, uint8_t n, ByteReader& br)
  {
    if (br.remaining() < 12) { br.skip(br.remaining()); return; }

    const uint32_t drawable = br.readU32();
    const uint32_t gcXid    = br.readU32();
    const int16_t x         = (int16_t)br.readU16();
    const int16_t y         = (int16_t)br.readU16();

    if (br.remaining() < n) { br.skip(br.remaining()); return; }
    std::vector<uint8_t> text(n);
    for (uint8_t i = 0; i < n; i++) text[i] = br.readU8();
    br.skip(br.remaining());

    // Verbose OR_TEXT trace for ImageText8 removed (was diagnostic-only for v1.10.4–v1.10.7).
#if X11_TRACE_FONT_ENABLED
    {
      // Diagnostic: show text drawn into each drawable with window geometry
      std::string txt(text.begin(), text.end());
      x11::WindowView wv{};
      bool isWin = ctx.windows().snapshot(drawable, wv);
      if (isWin) {
        TS_FPRINTF("[LABEL] ImageText8 drawable=0x%08X parent=0x%08X pos=(%d,%d) size=%ux%u text=\"%s\" at (%d,%d)\n",
                (unsigned)drawable, (unsigned)wv.parent_xid,
                (int)wv.x, (int)wv.y, (unsigned)wv.w, (unsigned)wv.h,
                txt.c_str(), (int)x, (int)y);
      } else {
        TS_FPRINTF("[LABEL] ImageText8 drawable=0x%08X (pixmap) text=\"%s\" at (%d,%d)\n",
                (unsigned)drawable, txt.c_str(), (int)x, (int)y);
      }
    }
#endif

    x11::DrawableRW dst{};
    if (!x11::resolveDrawableRW(ctx, drawable, dst)) {
      if (!ctx.windows().exists(drawable) && !ctx.pixmaps().exists(drawable))
        ctx.transport().sendErrorCore(x11::error::BadDrawable, seq, drawable, x11::opcode::ImageText8);
      return;
    }
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) return;

    // Get GC
    x11::GCState gc{};
    (void)getGC(gcXid, gc);

    // Resolve font
    const x11::font::BdfFont* f = resolveFont(ctx, gc);
    if (!f) return;

  #ifdef X11_TRACE_VERBOSE
    TS_FPRINTF("[TEXT] drawable=0x%08X gc=0x%08X gc.font=0x%08X usingFont=\"%s\" bbx=%dx%d ascent=%d descent=%d\n",
            (unsigned)drawable, (unsigned)gcXid, (unsigned)gc.font,
            f ? f->name.c_str() : "<null>",
            f ? f->bbx_w : -1, f ? f->bbx_h : -1,
            f ? f->ascent : -1, f ? f->descent : -1);
  #endif

    const int fontAscent  = (f->ascent  > 0) ? f->ascent  : 12;
    const int fontDescent = (f->descent > 0) ? f->descent : 4;

    // Compute overall width (sum of advances)
    int overallW = 0;
    for (uint8_t ch : text) overallW += f->advanceFor((int)ch);

    const x11::GCState* gcClipIT = gc.has_clip ? &gc : nullptr;

    auto fillRect32 = [&](int rx, int ry, int rw, int rh, uint32_t color) {
      if (rw <= 0 || rh <= 0) return;

      int x0 = rx;
      int y0 = ry;
      int x1 = rx + rw;
      int y1 = ry + rh;

      if (x0 < 0) x0 = 0;
      if (y0 < 0) y0 = 0;
      if (x1 > (int)dst.w) x1 = (int)dst.w;
      if (y1 > (int)dst.h) y1 = (int)dst.h;
      if (x0 >= x1 || y0 >= y1) return;

      const uint32_t c = (color & 0x00FFFFFFu) | 0xFF000000u;

      auto fillSub = [&](int32_t fx0, int32_t fy0, int32_t fx1, int32_t fy1) {
        for (int32_t yy = fy0; yy < fy1; yy++) {
          uint32_t* row = dst.pixels32 + (size_t)yy * (size_t)dst.stridePixels;
          for (int32_t xx = fx0; xx < fx1; xx++) row[(size_t)xx] = c;
        }
      };
      x11::gcClipForEachRect(gc, x0, y0, x1, y1, fillSub);
    };

    auto drawGlyph1bpp32 = [&](int leftX, int topY, const x11::font::Glyph& g, uint32_t fg) {
      const int gw = g.bbx_w;
      const int gh = g.bbx_h;
      if (gw <= 0 || gh <= 0) return;

      const uint32_t fgO = (fg & 0x00FFFFFFu) | 0xFF000000u;

      const int srcStride = g.rowStrideBytes();
      if (srcStride <= 0) return;

      // Defensive: bitmap must contain at least srcStride * gh bytes.
      const size_t need = (size_t)srcStride * (size_t)gh;
      if (g.bitmap.size() < need) return;

      for (int yy = 0; yy < gh; yy++) {
        const int dy = topY + yy;
        if (dy < 0 || dy >= (int)dst.h) continue;

        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        const uint8_t* srow = g.bitmap.data() + (size_t)yy * (size_t)srcStride;

        for (int xx = 0; xx < gw; xx++) {
          const int dx = leftX + xx;
          if (dx < 0 || dx >= (int)dst.w) continue;

          const uint32_t bit = (uint32_t)xx;
          const uint32_t sByte = bit >> 3;
          const uint32_t sBit  = bit & 7u;
          const uint8_t  mask  = (uint8_t)(1u << (7u - sBit));   // MSBFirst
          const bool on = (srow[sByte] & mask) != 0;

          if (on) {
            if (gcClipIT && !x11::gcPointVisible(*gcClipIT, dx, dy)) continue;
            drow[(size_t)dx] = fgO;
          }
        }
      }
    };

    // Alpha-blended glyph rendering for antialiased CoreText fonts
    auto drawGlyphAlpha32 = [&](int leftX, int topY, const x11::font::Glyph& g, uint32_t fg) {
      const int gw = g.bbx_w, gh = g.bbx_h;
      if (gw <= 0 || gh <= 0) return;
      if ((int)g.alpha.size() < gw * gh) return;

      const uint8_t fgR = (uint8_t)((fg >> 16) & 0xFFu);
      const uint8_t fgG = (uint8_t)((fg >>  8) & 0xFFu);
      const uint8_t fgB = (uint8_t)( fg        & 0xFFu);

      for (int yy = 0; yy < gh; yy++) {
        const int dy = topY + yy;
        if (dy < 0 || dy >= (int)dst.h) continue;
        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        const uint8_t* arow = g.alpha.data() + (size_t)yy * (size_t)gw;
        for (int xx = 0; xx < gw; xx++) {
          const int dx = leftX + xx;
          if (dx < 0 || dx >= (int)dst.w) continue;
          const uint8_t a = arow[xx];
          if (a == 0) continue;
          if (gcClipIT && !x11::gcPointVisible(*gcClipIT, dx, dy)) continue;
          if (a == 255) {
            drow[(size_t)dx] = (fg & 0x00FFFFFFu) | 0xFF000000u;
          } else {
            const uint32_t bg = drow[(size_t)dx];
            const uint8_t bgR = (uint8_t)((bg >> 16) & 0xFFu);
            const uint8_t bgG = (uint8_t)((bg >>  8) & 0xFFu);
            const uint8_t bgB = (uint8_t)( bg        & 0xFFu);
            const uint8_t oR = (uint8_t)((fgR * a + bgR * (255u - a) + 127u) / 255u);
            const uint8_t oG = (uint8_t)((fgG * a + bgG * (255u - a) + 127u) / 255u);
            const uint8_t oB = (uint8_t)((fgB * a + bgB * (255u - a) + 127u) / 255u);
            drow[(size_t)dx] = 0xFF000000u | ((uint32_t)oR << 16) | ((uint32_t)oG << 8) | (uint32_t)oB;
          }
        }
      }
    };

    const bool useAA = x11::font::antialiasedFonts();

#if X11_TRACE_FONT_ENABLED
    TS_FPRINTF("[IT8_DBG] draw=0x%X baseline_y=%d fontAsc=%d fontDesc=%d "
            "bgH=%d dst=%ux%u font=\"%s\"\n",
            (unsigned)drawable, (int)y, fontAscent, fontDescent,
            fontAscent + fontDescent,
            (unsigned)dst.w, (unsigned)dst.h,
            f->name.c_str());
#endif

    // Background fill: [x .. x+overallW) × [y-fontAscent .. y+fontDescent-1]
    //
    // Per the X11 spec, the background rectangle height is fontAscent + fontDescent.
    // The background covers [y - fontAscent, y + fontDescent - 1] inclusive.
    // For CoreText fonts, we ensure fontDescent > maxGlyphDescent so the
    // background covers all glyph pixels.  Using fontAscent + fontDescent
    // (not +1) prevents overlap with the next line's background fill, which
    // otherwise overwrites the bottom row of descenders.
    const int bgH = fontAscent + fontDescent;

    fillRect32((int)x,
               (int)y - fontAscent,
               overallW,
               bgH,
               gc.bg);

    // Draw glyphs
    int penX = (int)x;
    for (uint8_t ch : text) {
      const x11::font::Glyph* g = resolveGlyph(f, ch);
      if (!g) { penX += f->advanceFor((int)ch); continue; }

      const int leftX = penX + g->bbx_xoff;
      const int topY  = (int)y - g->bbx_yoff - g->bbx_h;

      if (g->hasAlpha() && useAA)
        drawGlyphAlpha32(leftX, topY, *g, gc.fg);
      else
        drawGlyph1bpp32(leftX, topY, *g, gc.fg);

      penX += (g->dwidth != 0) ? g->dwidth : f->advanceFor((int)ch);
    }

    if (dst.isWindow) {
      damageOrDirty(ctx, drawable,
                    (int32_t)x, (int32_t)y - fontAscent,
                    (int32_t)overallW, (int32_t)bgH);
    }
  }

  // ---- 75: PolyText16 ----
  //
  // Identical to PolyText8 except each character is a CHAR2B (2 bytes, byte1||byte2).
  // TEXTITEM16:
  //   BYTE len       0..254 = number of CHAR2B chars, 255 = font change
  //   INT8 delta
  //   if len==255: CARD32 font
  //   else:        CHAR2B string[len]   (len * 2 bytes)
  void DrawOps::handlePolyText16(XProtoContext& ctx, uint16_t seq, ByteReader& br)
  {
    if (br.remaining() < 12) { br.skip(br.remaining()); return; }

    const uint32_t drawable = br.readU32();
    const uint32_t gcXid    = br.readU32();
    int32_t penX            = (int16_t)br.readU16();
    const int32_t baseY     = (int16_t)br.readU16();

    x11::DrawableRW dst{};
    if (!x11::resolveDrawableRW(ctx, drawable, dst)) {
      br.skip(br.remaining());
      if (!ctx.windows().exists(drawable) && !ctx.pixmaps().exists(drawable))
        ctx.transport().sendErrorCore(x11::error::BadDrawable, seq, drawable, x11::opcode::PolyText16);
      return;
    }
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) { br.skip(br.remaining()); return; }

    x11::GCState gc{};
    (void)getGC(gcXid, gc);

    const x11::font::BdfFont* f = resolveFont(ctx, gc);
    if (!f) { br.skip(br.remaining()); return; }

    const x11::GCState* gcClip = gc.has_clip ? &gc : nullptr;
    const uint8_t ptFn = (uint8_t)(gc.function & 0x0Fu);
    const uint32_t ptPm = gc.plane_mask;
    const bool ptFast = (ptFn == 3) && ((ptPm & 0x00FFFFFFu) == 0x00FFFFFFu);

    auto drawGlyph1bpp32 = [&](int leftX, int topY, const x11::font::Glyph& g, uint32_t fg) {
      const int gw = g.bbx_w;
      const int gh = g.bbx_h;
      if (gw <= 0 || gh <= 0) return;

      const uint32_t fgO = (fg & 0x00FFFFFFu) | 0xFF000000u;
      const int srcStride = g.rowStrideBytes();
      if (srcStride <= 0) return;
      const size_t need = (size_t)srcStride * (size_t)gh;
      if (g.bitmap.size() < need) return;

      for (int yy = 0; yy < gh; yy++) {
        const int dy = topY + yy;
        if (dy < 0 || dy >= (int)dst.h) continue;

        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        const uint8_t* srow = g.bitmap.data() + (size_t)yy * (size_t)srcStride;

        for (int xx = 0; xx < gw; xx++) {
          const int dx = leftX + xx;
          if (dx < 0 || dx >= (int)dst.w) continue;

          const uint32_t bit = (uint32_t)xx;
          const uint32_t sByte = bit >> 3;
          const uint32_t sBit  = bit & 7u;
          const uint8_t mask = (uint8_t)(1u << (7u - sBit));
          const bool on = (srow[sByte] & mask) != 0;

          if (on) {
            if (gcClip && !x11::gcPointVisible(*gcClip, dx, dy)) continue;
            if (ptFast) drow[(size_t)dx] = fgO;
            else        drow[(size_t)dx] = x11_apply_rop_argb(drow[(size_t)dx], fgO, ptFn, ptPm);
          }
        }
      }
    };

    // Alpha-blended glyph rendering for antialiased CoreText fonts
    auto drawGlyphAlpha32 = [&](int leftX, int topY, const x11::font::Glyph& g, uint32_t fg) {
      const int gw = g.bbx_w, gh = g.bbx_h;
      if (gw <= 0 || gh <= 0) return;
      if ((int)g.alpha.size() < gw * gh) return;

      const uint8_t fgR = (uint8_t)((fg >> 16) & 0xFFu);
      const uint8_t fgG = (uint8_t)((fg >>  8) & 0xFFu);
      const uint8_t fgB = (uint8_t)( fg        & 0xFFu);

      for (int yy = 0; yy < gh; yy++) {
        const int dy = topY + yy;
        if (dy < 0 || dy >= (int)dst.h) continue;
        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        const uint8_t* arow = g.alpha.data() + (size_t)yy * (size_t)gw;
        for (int xx = 0; xx < gw; xx++) {
          const int dx = leftX + xx;
          if (dx < 0 || dx >= (int)dst.w) continue;
          const uint8_t a = arow[xx];
          if (a == 0) continue;
          if (gcClip && !x11::gcPointVisible(*gcClip, dx, dy)) continue;
          if (a == 255) {
            drow[(size_t)dx] = (fg & 0x00FFFFFFu) | 0xFF000000u;
          } else {
            const uint32_t bg = drow[(size_t)dx];
            const uint8_t bgR = (uint8_t)((bg >> 16) & 0xFFu);
            const uint8_t bgG = (uint8_t)((bg >>  8) & 0xFFu);
            const uint8_t bgB = (uint8_t)( bg        & 0xFFu);
            const uint8_t oR = (uint8_t)((fgR * a + bgR * (255u - a) + 127u) / 255u);
            const uint8_t oG = (uint8_t)((fgG * a + bgG * (255u - a) + 127u) / 255u);
            const uint8_t oB = (uint8_t)((fgB * a + bgB * (255u - a) + 127u) / 255u);
            drow[(size_t)dx] = 0xFF000000u | ((uint32_t)oR << 16) | ((uint32_t)oG << 8) | (uint32_t)oB;
          }
        }
      }
    };

    const bool useAA = x11::font::antialiasedFonts();

    while (br.remaining() >= 2) {
      const uint8_t len = br.readU8();
      const int8_t  delta = (int8_t)br.readU8();

      penX += (int32_t)delta;

      if (len == 0) continue;

      if (len == 255) {
        if (br.remaining() < 4) { br.skip(br.remaining()); break; }
        const uint32_t newFont = br.readU32();
        gc.font = newFont;
        const x11::font::BdfFont* nf = resolveFont(ctx, gc);
        if (nf) f = nf;
        continue;
      }

      // Each CHAR2B is 2 bytes
      if (br.remaining() < (size_t)len * 2u) { br.skip(br.remaining()); break; }

      for (uint8_t i = 0; i < len; i++) {
        // CHAR2B: byte1 (high) then byte2 (low)
        const uint8_t byte1 = br.readU8();
        const uint8_t byte2 = br.readU8();
        const int encoding = ((int)byte1 << 8) | (int)byte2;

        const x11::font::Glyph* g = f->getGlyph(encoding);
        if (!g) g = f->getGlyph(f->defaultChar);
        if (!g) g = f->getGlyph((int)'?');
        if (!g) { penX += f->advanceFor(encoding); continue; }

        const int leftX = (int)penX + g->bbx_xoff;
        const int topY  = (int)baseY - g->bbx_yoff - g->bbx_h;

        if (g->hasAlpha() && useAA)
          drawGlyphAlpha32(leftX, topY, *g, gc.fg);
        else
          drawGlyph1bpp32(leftX, topY, *g, gc.fg);

        penX += (g->dwidth != 0) ? g->dwidth : f->advanceFor(encoding);
      }
    }

    br.skip(br.remaining());

    if (dst.isWindow) {
      damageOrDirty(ctx, drawable, 0, 0, (int32_t)dst.w, (int32_t)dst.h);
    }
  }


  // ---- 77: ImageText16 ----
  //
  // Identical to ImageText8 except each character is a CHAR2B (2 bytes, byte1||byte2).
  // n (from minor/data byte) is the number of CHAR2B characters (not bytes).
  void DrawOps::handleImageText16(XProtoContext& ctx, uint16_t seq, uint8_t n, ByteReader& br)
  {
    if (br.remaining() < 12) { br.skip(br.remaining()); return; }

    const uint32_t drawable = br.readU32();
    const uint32_t gcXid    = br.readU32();
    const int16_t x         = (int16_t)br.readU16();
    const int16_t y         = (int16_t)br.readU16();

    if (br.remaining() < (size_t)n * 2u) { br.skip(br.remaining()); return; }
    std::vector<int> text(n);
    for (uint8_t i = 0; i < n; i++) {
      const uint8_t byte1 = br.readU8();
      const uint8_t byte2 = br.readU8();
      text[i] = ((int)byte1 << 8) | (int)byte2;
    }
    br.skip(br.remaining());

    x11::DrawableRW dst{};
    if (!x11::resolveDrawableRW(ctx, drawable, dst)) {
      if (!ctx.windows().exists(drawable) && !ctx.pixmaps().exists(drawable))
        ctx.transport().sendErrorCore(x11::error::BadDrawable, seq, drawable, x11::opcode::ImageText16);
      return;
    }
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) return;

    x11::GCState gc{};
    (void)getGC(gcXid, gc);

    const x11::font::BdfFont* f = resolveFont(ctx, gc);
    if (!f) return;

    const int fontAscent  = (f->ascent  > 0) ? f->ascent  : 12;
    const int fontDescent = (f->descent > 0) ? f->descent : 4;

    // Compute overall width
    int overallW = 0;
    for (int ch : text) overallW += f->advanceFor(ch);

    const x11::GCState* gcClipIT = gc.has_clip ? &gc : nullptr;

    auto fillRect32 = [&](int rx, int ry, int rw, int rh, uint32_t color) {
      if (rw <= 0 || rh <= 0) return;

      int x0 = rx, y0 = ry, x1 = rx + rw, y1 = ry + rh;
      if (x0 < 0) x0 = 0;
      if (y0 < 0) y0 = 0;
      if (x1 > (int)dst.w) x1 = (int)dst.w;
      if (y1 > (int)dst.h) y1 = (int)dst.h;
      if (x0 >= x1 || y0 >= y1) return;

      const uint32_t c = (color & 0x00FFFFFFu) | 0xFF000000u;

      auto fillSub = [&](int32_t fx0, int32_t fy0, int32_t fx1, int32_t fy1) {
        for (int32_t yy = fy0; yy < fy1; yy++) {
          uint32_t* row = dst.pixels32 + (size_t)yy * (size_t)dst.stridePixels;
          for (int32_t xx = fx0; xx < fx1; xx++) row[(size_t)xx] = c;
        }
      };
      x11::gcClipForEachRect(gc, x0, y0, x1, y1, fillSub);
    };

    auto drawGlyph1bpp32 = [&](int leftX, int topY, const x11::font::Glyph& g, uint32_t fg) {
      const int gw = g.bbx_w;
      const int gh = g.bbx_h;
      if (gw <= 0 || gh <= 0) return;

      const uint32_t fgO = (fg & 0x00FFFFFFu) | 0xFF000000u;
      const int srcStride = g.rowStrideBytes();
      if (srcStride <= 0) return;
      const size_t need = (size_t)srcStride * (size_t)gh;
      if (g.bitmap.size() < need) return;

      for (int yy = 0; yy < gh; yy++) {
        const int dy = topY + yy;
        if (dy < 0 || dy >= (int)dst.h) continue;

        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        const uint8_t* srow = g.bitmap.data() + (size_t)yy * (size_t)srcStride;

        for (int xx = 0; xx < gw; xx++) {
          const int dx = leftX + xx;
          if (dx < 0 || dx >= (int)dst.w) continue;

          const uint32_t bit = (uint32_t)xx;
          const uint32_t sByte = bit >> 3;
          const uint32_t sBit  = bit & 7u;
          const uint8_t  mask  = (uint8_t)(1u << (7u - sBit));
          const bool on = (srow[sByte] & mask) != 0;

          if (on) {
            if (gcClipIT && !x11::gcPointVisible(*gcClipIT, dx, dy)) continue;
            drow[(size_t)dx] = fgO;
          }
        }
      }
    };

    // Alpha-blended glyph rendering for antialiased CoreText fonts
    auto drawGlyphAlpha32 = [&](int leftX, int topY, const x11::font::Glyph& g, uint32_t fg) {
      const int gw = g.bbx_w, gh = g.bbx_h;
      if (gw <= 0 || gh <= 0) return;
      if ((int)g.alpha.size() < gw * gh) return;

      const uint8_t fgR = (uint8_t)((fg >> 16) & 0xFFu);
      const uint8_t fgG = (uint8_t)((fg >>  8) & 0xFFu);
      const uint8_t fgB = (uint8_t)( fg        & 0xFFu);

      for (int yy = 0; yy < gh; yy++) {
        const int dy = topY + yy;
        if (dy < 0 || dy >= (int)dst.h) continue;
        uint32_t* drow = dst.pixels32 + (size_t)dy * (size_t)dst.stridePixels;
        const uint8_t* arow = g.alpha.data() + (size_t)yy * (size_t)gw;
        for (int xx = 0; xx < gw; xx++) {
          const int dx = leftX + xx;
          if (dx < 0 || dx >= (int)dst.w) continue;
          const uint8_t a = arow[xx];
          if (a == 0) continue;
          if (gcClipIT && !x11::gcPointVisible(*gcClipIT, dx, dy)) continue;
          if (a == 255) {
            drow[(size_t)dx] = (fg & 0x00FFFFFFu) | 0xFF000000u;
          } else {
            const uint32_t bg = drow[(size_t)dx];
            const uint8_t bgR = (uint8_t)((bg >> 16) & 0xFFu);
            const uint8_t bgG = (uint8_t)((bg >>  8) & 0xFFu);
            const uint8_t bgB = (uint8_t)( bg        & 0xFFu);
            const uint8_t oR = (uint8_t)((fgR * a + bgR * (255u - a) + 127u) / 255u);
            const uint8_t oG = (uint8_t)((fgG * a + bgG * (255u - a) + 127u) / 255u);
            const uint8_t oB = (uint8_t)((fgB * a + bgB * (255u - a) + 127u) / 255u);
            drow[(size_t)dx] = 0xFF000000u | ((uint32_t)oR << 16) | ((uint32_t)oG << 8) | (uint32_t)oB;
          }
        }
      }
    };

    const bool useAA = x11::font::antialiasedFonts();

    // Background fill (see ImageText8 for rationale)
    const int bgH = fontAscent + fontDescent;
    fillRect32((int)x, (int)y - fontAscent, overallW, bgH, gc.bg);

    // Draw glyphs
    int penX = (int)x;
    for (int ch : text) {
      const x11::font::Glyph* g = f->getGlyph(ch);
      if (!g) g = f->getGlyph(f->defaultChar);
      if (!g) g = f->getGlyph((int)'?');
      if (!g) { penX += f->advanceFor(ch); continue; }

      const int leftX = penX + g->bbx_xoff;
      const int topY  = (int)y - g->bbx_yoff - g->bbx_h;

      if (g->hasAlpha() && useAA)
        drawGlyphAlpha32(leftX, topY, *g, gc.fg);
      else
        drawGlyph1bpp32(leftX, topY, *g, gc.fg);

      penX += (g->dwidth != 0) ? g->dwidth : f->advanceFor(ch);
    }

    if (dst.isWindow) {
      damageOrDirty(ctx, drawable,
                    (int32_t)x, (int32_t)y - fontAscent,
                    (int32_t)overallW, (int32_t)bgH);
    }
  }

} // namespace x11
