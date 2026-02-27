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
#include "Core/FontTable.hpp"
#include "Utils/WireEvents.hpp"
#include "Utils/RasterOp.hpp"
#include "Core/SurfaceDesc.hpp"
#include "Core/DrawableSurfaceRegistry.hpp"

// bridging
#include "x11_requests.h"
#include "XProtoServerBridge.h"
#include "Core/Font8x8.hpp"
#include "Utils/WireLE.hpp"
#include "Ops/ReplyWriter.hpp"

// util
#include "Damage.hpp"

namespace x11 {

static constexpr uint32_t kBitmapScanlinePadBits = 32;   // matches SetupSuccess
static constexpr bool     kBitmapBitOrderLSBFirst = true;

// Core event: NoExpose (type 14)
// Sent after CopyArea/CopyPlane when GC.graphics_exposures is true
static inline void sendNoExpose(x11::XProtoContext& ctx,
                                uint32_t dstWid,
                                uint16_t seq,
                                uint8_t majorEvent,
                                uint16_t minorEvent)
{
  uint8_t ev[32];
  std::memset(ev, 0, sizeof(ev));
  ev[0] = 14; // NoExpose
  // ev[1] unused
  x11::wire::wr16_le(ev + 2, seq);
  x11::wire::wr32_le(ev + 4, dstWid);      // drawable
  x11::wire::wr16_le(ev + 8, minorEvent);  // minorEvent
  ev[10] = majorEvent;                     // majorEvent (e.g. 62 CopyArea)

  // Route as an event to the destination window (same client)
  (void)ctx.transport().sendEvent32(dstWid, ev);
}

  
DrawOps::DrawOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::ClearArea, &DrawOps::onMajor, this);  // 61
  reg.registerMajor(x11::opcode::CopyArea,  &DrawOps::onMajor, this);  // 62
  reg.registerMajor(x11::opcode::CopyPlane, &DrawOps::onMajor, this);  // 63
  reg.registerMajor(x11::opcode::PutImage,  &DrawOps::onMajor, this);  // 72
  reg.registerMajor(x11::opcode::GetImage,  &DrawOps::onMajor, this);  // 73
  reg.registerMajor(x11::opcode::PolyText8,  &DrawOps::onMajor, this); // 74
  reg.registerMajor(x11::opcode::ImageText8, &DrawOps::onMajor, this); // 76
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
    case x11::opcode::PolyText8:  handlePolyText8(ctx, dc.seq, dc.br); return; // 74
    case x11::opcode::ImageText8: handleImageText8(ctx, dc.seq, dc.minor /*n*/, dc.br); return; // 76, dc.minor is string length
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

static inline void drawGlyph1bppBGRA(uint32_t* pix, uint32_t fbW, uint32_t fbH,
                                     int dstLeftX, int dstTopY,
                                     const x11::font::Glyph& g,
                                     uint32_t fg)
{
  const int bw = g.bbx_w;
  const int bh = g.bbx_h;
  if (bw <= 0 || bh <= 0) return;

  const int stride = g.rowStrideBytes();
  const uint8_t* bm = g.bitmap.data();
  if (!bm) return;

  for (int gy = 0; gy < bh; gy++) {
    const int yy = dstTopY + gy;
    if ((unsigned)yy >= fbH) continue;

    const uint8_t* row = bm + gy * stride;

    for (int gx = 0; gx < bw; gx++) {
      const int xx = dstLeftX + gx;
      if ((unsigned)xx >= fbW) continue;

      const uint8_t byte = row[gx >> 3];
      const uint8_t bit  = 0x80u >> (gx & 7);
      if (byte & bit) {
        pix[(size_t)yy * fbW + (size_t)xx] = fg;
      }
    }
  }
}  


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
  
  
static inline int clampi(int v, int lo, int hi) {
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

  
  
  

// -----------------------------
// PutImage (major 72)
// format: dc.minor (1=XYPixmap, 2=ZPixmap)
// We implement the xeyes-critical case first:
//   format=1, depth=1, destination is a depth-1 pixmap (packed bits).
// -----------------------------
void DrawOps::handlePutImage(XProtoContext& ctx, uint16_t /*seq*/, uint8_t format, ByteReader& br) {
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
  (void)br.readU32(); // gc (unused for now)

  const uint16_t width  = br.readU16();
  const uint16_t height = br.readU16();
  const int16_t dstX = br.readI16();
  const int16_t dstY = br.readI16();

  const uint8_t leftPad = br.readU8();
  if (leftPad > 7) { br.skip(br.remaining()); return; }

  const uint8_t depth   = br.readU8();
  br.skip(2); // pad0/pad1

  if (width == 0 || height == 0) { br.skip(br.remaining()); return; }

  // Is destination a window?
  const bool dstIsWindow = ctx.windows().exists(drawable);

  // ============================================================================================
  // WINDOW path: ZPixmap depth {24,32} -> SurfaceRegistry (keyed by host/top-level window)
  // ============================================================================================
  if (dstIsWindow) {
    // Only accept ZPixmap for window surfaces right now.
    // X11 core: format 2 == ZPixmap
    if (format != 2 || (depth != 24 && depth != 32)) {
      br.skip(br.remaining());
      return;
    }

    // Route window drawing to top-level host (current rootless model).
    const uint32_t host = ctx.windows().topLevelAncestorOf(drawable);
    const uint32_t key  = host ? host : drawable;

    // Surface must exist (Swift-owned backing store).
    x11::SurfaceDesc surf{};
    if (!ctx.surfaces().get(key, surf) ||
        !surf.ptr || surf.bytesPerRow == 0 || surf.w == 0 || surf.h == 0) {
      // Transitional behavior: if surface isn't installed yet, drop quietly.
      br.skip(br.remaining());
      return;
    }

    // Compute drawable's absolute offset within host by walking up the tree.
    int32_t offX = 0;
    int32_t offY = 0;
    if (drawable != key) {
      uint32_t cur = drawable;
      for (int hop = 0; hop < 256 && cur && cur != key; hop++) {
        x11::WindowView vw{};
        if (!ctx.windows().snapshot(cur, vw)) break;
        offX += (int32_t)vw.x;
        offY += (int32_t)vw.y;
        cur = vw.parent_xid;
      }
    }

    const int32_t baseX = (int32_t)dstX + offX;
    const int32_t baseY = (int32_t)dstY + offY;

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

    uint8_t* dstBase = static_cast<uint8_t*>(surf.ptr);

#ifndef NDEBUG
    ctx.tracef("[PutImage/WIN] drawable=0x%08X host=0x%08X off=%d,%d dst=%d,%d wh=%u,%u depth=%u fmt=%u surf=%ux%u bpr=%u\n",
               (unsigned)drawable, (unsigned)key,
               (int)offX, (int)offY,
               (int)dstX, (int)dstY,
               (unsigned)width, (unsigned)height,
               (unsigned)depth, (unsigned)format,
               (unsigned)surf.w, (unsigned)surf.h, (unsigned)surf.bytesPerRow);
#endif

    // Copy rows, clipped to surface bounds.
    for (uint16_t yy = 0; yy < height; yy++) {
      const int32_t dy = baseY + (int32_t)yy;
      if (dy < 0 || dy >= (int32_t)surf.h) continue;

      const uint8_t* srow = src + (size_t)yy * (size_t)srcStride;
      uint8_t* drow = dstBase + (size_t)dy * (size_t)surf.bytesPerRow;

      int32_t x0 = baseX;
      int32_t x1 = baseX + (int32_t)width;
      if (x0 < 0) x0 = 0;
      if (x1 > (int32_t)surf.w) x1 = (int32_t)surf.w;
      if (x1 <= x0) continue;

      const int32_t copyPx = x1 - x0;
      const int32_t srcX0  = x0 - baseX;

      std::memcpy(drow + (size_t)x0 * 4u,
                  srow + (size_t)srcX0 * 4u,
                  (size_t)copyPx * 4u);
    }

    // Damage the drawn region (drawable-local coords; damageOrDirty translates to host).
    damageOrDirty(ctx, drawable, (int32_t)dstX, (int32_t)dstY, (int32_t)width, (int32_t)height);
    return;
  }

  // ============================================================================================
  // PIXMAP path: XYPixmap depth=1 (existing behavior)
  // ============================================================================================

  // Only implement XYPixmap depth=1 right now for pixmaps.
  if (format != 1 || depth != 1) {
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
  const uint32_t planeMask = br.readU32();
  br.skip(br.remaining());

  if (w == 0 || h == 0) return;

  // Only support ZPixmap (format 2) for now
  if (format != 2) {
    // Send error or empty reply? For bring-up, just skip.
    return;
  }

  // Resolve drawable
  x11::DrawableRW src{};
  if (!x11::resolveDrawableRW(ctx, drawable, src) || !src.pixels32) return;
  if (src.w == 0 || src.h == 0 || src.stridePixels == 0) return;

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
  // Copy / ROP with overlap-safe ordering
  // ------------------------------------------------------------
  auto rowCopyFast = [&](int sy, int dy) {
    const uint32_t* sp = srcPixels + (size_t)sy * (size_t)srcStride + (size_t)sx0;
    uint32_t*       dp = dstPixels + (size_t)dy * (size_t)dstStride + (size_t)dx0;
    std::memmove(dp, sp, (size_t)cw * sizeof(uint32_t));
    for (int i = 0; i < cw; i++) dp[i] = (dp[i] & 0x00FFFFFFu) | 0xFF000000u;
  };

  auto rowRop = [&](int sy, int dy, bool rightToLeft) {
    const uint32_t* sp = srcPixels + (size_t)sy * (size_t)srcStride + (size_t)sx0;
    uint32_t*       dp = dstPixels + (size_t)dy * (size_t)dstStride + (size_t)dx0;

    if (!rightToLeft) {
      for (int i = 0; i < cw; i++) {
        uint32_t out = x11_apply_rop_argb(dp[i], sp[i], fn, gc.plane_mask);
        dp[i] = (out & 0x00FFFFFFu) | 0xFF000000u;
      }
    } else {
      for (int i = cw - 1; i >= 0; i--) {
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

    if (canMemmoveFast) {
      rowCopyFast(sy, dy);
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

  // ------------------------------------------------------------
  // After successful copy: send NoExpose (bring-up)
  // ------------------------------------------------------------
  if (dstIsWin) {
    auto ev = x11::wireev::buildNoExpose(seq, dst,
                                        x11::opcode::CopyArea, 0);
    (void)ctx.transport().sendEvent32(dst, ev.data());
  }
}
  
// -----------------------------
// CopyPlane (major 63)
// -----------------------------
  void DrawOps::handleCopyPlane(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br)
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
    // else keep defaults
    
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
          dstPixels[(size_t)dy * (size_t)dstStridePx + (size_t)dx] = on ? fg : bg;
        }
      }
    }
    
    // Damage only if destination is a window.
    if ( dstIsWindow ) {
      damageOrDirty(ctx, dst, (int32_t)dstX, (int32_t)dstY, (int32_t)wpx, (int32_t)hpx);
    }
  }
  
  
  void DrawOps::handleClearArea(XProtoContext& ctx, uint16_t /*seq*/, uint8_t exposures, ByteReader& br)
  {
    if (br.remaining() < 12) { br.skip(br.remaining()); return; }

    const uint32_t wid = br.readU32();
    const int16_t  x   = br.readI16();
    const int16_t  y   = br.readI16();
    const uint16_t wpx = br.readU16();
    const uint16_t hpx = br.readU16();
    br.skip(br.remaining());

  #ifdef X11_TRACE_VERBOSE
    if (wid == 0x10000012u) {
      fprintf(stderr,
              "[ClearArea] wid=0x%08X x=%d y=%d w=%u h=%u exposures=%u\n",
              (unsigned)wid,
              (int)x, (int)y, (unsigned)wpx, (unsigned)hpx,
              (unsigned)exposures);
    }
  #endif

    if (wid == 0) return;
    if (!ctx.windows().exists(wid)) return;

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

    // X11 spec: ClearArea clears to the window's background_pixel.
    // Fall back to opaque white if no background was set (legacy bring-up behavior).
    uint32_t bg = 0xFFFFFFFFu;
    {
      x11::WindowView wv{};
      if (ctx.windows().snapshot(wid, wv) && wv.has_background_pixel) {
        bg = wv.background_pixel;
      }
    }

    for (int yy = y0; yy < y1; yy++) {
      uint32_t* row = dst.pixels32 + (size_t)yy * (size_t)dst.stridePixels;
      for (int xx = x0; xx < x1; xx++) row[(size_t)xx] = bg;
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
  void DrawOps::handlePolyText8(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br)
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

    x11::DrawableRW dst{};
    if (!x11::resolveDrawableRW(ctx, drawable, dst)) { br.skip(br.remaining()); return; }
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) { br.skip(br.remaining()); return; }

    // Get GC
    x11::GCState gc{};
    (void)getGC(gcXid, gc);

    // Resolve font from GC
    const x11::font::BdfFont* f = resolveFont(ctx, gc);
    if (!f) { br.skip(br.remaining()); return; }

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

          if (on) drow[(size_t)dx] = fgO;
        }
      }
    };

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

      for (uint8_t i = 0; i < len; i++) {
        const uint8_t ch = br.readU8();

        const x11::font::Glyph* g = resolveGlyph(f, ch);
        if (!g) { penX += f->advanceFor((int)ch); continue; }

        const int leftX = (int)penX + g->bbx_xoff;
        const int topY  = (int)baseY - g->bbx_yoff - (g->bbx_h - 1);

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
  void DrawOps::handleImageText8(XProtoContext& ctx, uint16_t /*seq*/, uint8_t n, ByteReader& br)
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

    x11::DrawableRW dst{};
    if (!x11::resolveDrawableRW(ctx, drawable, dst)) return;
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0 || dst.stridePixels == 0) return;

    // Get GC
    x11::GCState gc{};
    (void)getGC(gcXid, gc);

    // Resolve font
    const x11::font::BdfFont* f = resolveFont(ctx, gc);
    if (!f) return;

  #ifdef X11_TRACE_VERBOSE
    fprintf(stderr, "[TEXT] drawable=0x%08X gc=0x%08X gc.font=0x%08X usingFont=\"%s\" bbx=%dx%d ascent=%d descent=%d\n",
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

      for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = dst.pixels32 + (size_t)yy * (size_t)dst.stridePixels;
        for (int xx = x0; xx < x1; xx++) row[(size_t)xx] = c;
      }
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
          //const uint32_t sByte = bit >> 3;
          //const uint32_t sBit  = bit & 7u;

          const uint32_t sByte = bit >> 3;
          const uint32_t sBit  = bit & 7u;
          const uint8_t  mask  = (uint8_t)(1u << (7u - sBit));   // MSBFirst
          const bool on = (srow[sByte] & mask) != 0;
          
          // LSBFirst bit order, consistent with your other paths.
          //const bool on = (srow[sByte] & (uint8_t)(1u << sBit)) != 0;
          if (on) drow[(size_t)dx] = fgO;
        }
      }
    };

    // Background fill: [x .. x+overallW) × [y-ascent .. y+descent)
    fillRect32((int)x,
               (int)y - fontAscent,
               overallW,
               fontAscent + fontDescent,
               gc.bg);

    // Draw glyphs
    int penX = (int)x;
    for (uint8_t ch : text) {
      const x11::font::Glyph* g = resolveGlyph(f, ch);
      if (!g) { penX += f->advanceFor((int)ch); continue; }

      const int leftX = penX + g->bbx_xoff;
      const int topY  = (int)y - g->bbx_yoff - (g->bbx_h - 1);

      drawGlyph1bpp32(leftX, topY, *g, gc.fg);

      penX += (g->dwidth != 0) ? g->dwidth : f->advanceFor((int)ch);
    }

    if (dst.isWindow) {
      // Background fill rect is the largest affected region.
      damageOrDirty(ctx, drawable,
                    (int32_t)x, (int32_t)y - fontAscent,
                    (int32_t)overallW, (int32_t)(fontAscent + fontDescent));
    }
  }

} // namespace x11
