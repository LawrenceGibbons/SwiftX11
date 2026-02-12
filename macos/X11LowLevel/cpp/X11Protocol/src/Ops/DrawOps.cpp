//
//  DrawOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "Ops/DrawOps.hpp"

#include <cstddef>
#include <cstdint>

#include "Core/XProtoContext.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Utils/ByteReader.hpp"
#include "Core/PixmapTable.hpp"   // adjust include path to your project
#include "Core/WindowTable.hpp"
#include "Core/GCTable.hpp"
#include "Core/DrawableRW.hpp"
#include "Ops/DrawOps.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "UI/UICommandQueue.hpp"

// bridging
#include "x11_backend_fb.h"
#include "x11_requests.h"
#include "XProtoServerBridge.h"

// util
#include "Damage.hpp"

namespace x11 {

static constexpr uint32_t kBitmapScanlinePadBits = 32;   // matches SetupSuccess
static constexpr bool     kBitmapBitOrderLSBFirst = true;

DrawOps::DrawOps(XProtoRegistrar& reg) {
  reg.registerMajor(61, &DrawOps::onMajor, this); // ClearArea
  reg.registerMajor(62, &DrawOps::onMajor, this); // CopyArea
  reg.registerMajor(63, &DrawOps::onMajor, this); // CopyPlane
  reg.registerMajor(72, &DrawOps::onMajor, this); // PutImage
}

void DrawOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<DrawOps*>(user)->handle(ctx, dc);
}

void DrawOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case 61: handleClearArea(ctx, dc.seq, dc.minor /*exposures*/, dc.br); return;
    case 62: handleCopyArea(ctx, dc.seq, dc.br); return;
    case 63: handleCopyPlane(ctx, dc.seq, dc.br); return;
    case 72: handlePutImage(ctx, dc.seq, dc.minor /*format*/, dc.br); return;
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
  (void)br.readU32(); // gc (unused in depth1->pixmap path)

  const uint16_t width  = br.readU16();
  const uint16_t height = br.readU16();
  const int16_t  dstX   = (int16_t)br.readU16();
  const int16_t  dstY   = (int16_t)br.readU16();

  const uint8_t leftPad = br.readU8();
  const uint8_t depth   = br.readU8();
  br.skip(2); // pad0/pad1

  if (width == 0 || height == 0) { br.skip(br.remaining()); return; }

  // Only implement XYPixmap depth=1 right now.
  if (format != 1 || depth != 1) {
    br.skip(br.remaining());
    return;
  }

  const bool dstIsWindow = ctx.windows().exists(drawable);

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
    // truncated request body
    br.skip(br.remaining());
    return;
  }
  const std::size_t need = (std::size_t)need64;

  const uint8_t* src = br.ptr();   // raw packed bitmap data
  br.skip(br.remaining());         // consume rest (including request padding)

  // xxx temp ---
  ctx.tracef("[PutImage] drawable=0x%08X dstIsWindow=%d w=%u h=%u depth=%u fmt=%u\n",
             drawable, dstIsWindow ? 1 : 0, pw, ph, depth, format);
  // xxx --- temp
  
  // Copy bits from src into dstBits (both LSBFirst as per SetupSuccess).
  // NOTE: PutImage uses leftPad in *source bit indexing*.
  for (uint16_t yy = 0; yy < height; yy++) {
    const int32_t dy = (int32_t)dstY + (int32_t)yy;
    if (dy < 0 || dy >= (int32_t)ph) continue;

    const uint8_t* srow = src + (std::size_t)yy * (std::size_t)srcStride;

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
      const uint32_t dByte = ((uint32_t)dx) >> 3;
      const uint32_t dBit  = ((uint32_t)dx) & 7u;
      const uint8_t  dmask = kBitmapBitOrderLSBFirst ? (uint8_t)(1u << dBit) : (uint8_t)(1u << (7u - dBit));

      uint8_t* drow = dstBits + (std::size_t)dy * (std::size_t)dstStride;
      if (on) drow[dByte] |= dmask;
      else    drow[dByte] &= (uint8_t)~dmask;
    }
  }

// xxx maybe temp  if (dstIsWindow) {
// xxx maybe temp    ctx.tracef("[DAMAGE_ROUTE] PutImage wid=%d stays unrouted\n", drawable);
// xxx maybe temp    if (ctx.windows().isReadyToPresent(drawable)) {
// xxx maybe temp      // IMPORTANT: damage must be attributed to the drawable that changed (drawable),
// xxx maybe temp      // NOT the host. Swift will map wid -> host and choose source correctly.
// xxx maybe temp      x11_requests_push_damage(drawable);
// xxx maybe temp    } else {
// xxx maybe temp      ctx.windows().markDirty(drawable);
// xxx maybe temp    }
// xxx maybe temp  }
  // xxx maybe temp ---- 
  if (dstIsWindow) {
    x11_requests_push_damage(drawable);
  }
  // xxx ---- maybe temp 
  
  (void)need;
}


  
// -----------------------------
// CopyArea (major 62)
// -----------------------------
  void DrawOps::handleCopyArea(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
    // Body after 4-byte header (24 bytes):
    //   CARD32 src
    //   CARD32 dst
    //   CARD32 gc      (ignored for now; CopyArea is raw pixel copy)
    //   INT16  srcX
    //   INT16  srcY
    //   INT16  dstX
    //   INT16  dstY
    //   CARD16 w
    //   CARD16 h
    if (br.remaining() < 24) { br.skip(br.remaining()); return; }

    const uint32_t src = br.readU32();
    const uint32_t dst = br.readU32();
    (void)br.readU32(); // gc

    const int16_t srcX = (int16_t)br.readU16();
    const int16_t srcY = (int16_t)br.readU16();
    const int16_t dstX = (int16_t)br.readU16();
    const int16_t dstY = (int16_t)br.readU16();

    const uint16_t wpx = br.readU16();
    const uint16_t hpx = br.readU16();
    br.skip(br.remaining());

    if (wpx == 0 || hpx == 0) return;

    // ------------------------------------------------------------
    // Resolve source drawable -> read-only pixel pointer + dimensions
    // ------------------------------------------------------------
    const uint32_t* srcPixels = nullptr;
    int srcW = 0, srcH = 0;

    const bool srcIsWin = ctx.windows().exists(src);
    const bool srcIsPix = ctx.pixmaps().exists(src);

    // 1) Window source => C framebuffer
    if (srcIsWin) {
      uint32_t* wPixels = nullptr;
      uint32_t wW = 0, wH = 0;
      if (!x11_xproto_window_fb_rw(src, &wPixels, &wW, &wH) || !wPixels) return;
      srcPixels = wPixels;
      srcW = (int)wW;
      srcH = (int)wH;
      // 2) Pixmap source => C++ PixmapTable
    } else if (srcIsPix) {
      PixmapView pv{};
      if (!ctx.pixmaps().snapshot(src, pv)) return;
      if (pv.depth == 1) return;              // CopyArea not for depth-1 masks
      if (!pv.pixels) return;
      srcPixels = pv.pixels;
      srcW = (int)pv.w;
      srcH = (int)pv.h;
    } else {
      return; // unknown drawable
    }
    
    // ------------------------------------------------------------
    // Resolve destination drawable -> writable pixel pointer + dimensions
    // ------------------------------------------------------------
    uint32_t* dstPixels = nullptr;
    int dstW = 0, dstH = 0;
    const bool dstIsWin = ctx.windows().exists(dst);
    const bool dstIsPix = ctx.pixmaps().exists(dst);

    bool dstIsWindow = false;

    if (dstIsWin) {
      uint32_t* wPixels = nullptr;
      uint32_t wW = 0, wH = 0;
      if (!x11_xproto_window_fb_rw(dst, &wPixels, &wW, &wH) || !wPixels) return;
      dstPixels = wPixels;
      dstW = (int)wW;
      dstH = (int)wH;
      dstIsWindow = true;
    } else if (dstIsPix) {
      uint16_t pw = 0, ph = 0;
      dstPixels = ctx.pixmaps().mutablePixels(dst, &pw, &ph);
      if (!dstPixels) return;
      dstW = (int)pw;
      dstH = (int)ph;
      dstIsWindow = false;
    } else {
      return; // unknown drawable
    }
    
    // ------------------------------------------------------------
    // Blit with clamp (same as your C implementation)
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

        dstPixels[(size_t)dy * (size_t)dstW + (size_t)dx] =
          srcPixels[(size_t)sy * (size_t)srcW + (size_t)sx];
      }
    }

    // ------------------------------------------------------------
    // Damage only if destination is a window (pixmaps present when copied into a window)
    // ------------------------------------------------------------
    damageOrDirty(ctx, dst );
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
    
    const int16_t srcX = (int16_t)br.readU16();
    const int16_t srcY = (int16_t)br.readU16();
    const int16_t dstX = (int16_t)br.readU16();
    const int16_t dstY = (int16_t)br.readU16();
    
    const uint16_t wpx = br.readU16();
    const uint16_t hpx = br.readU16();
    
    const uint32_t bitPlane = br.readU32();
    br.skip(br.remaining());
    
    if (wpx == 0 || hpx == 0) return;
    
    // Minimal: only support plane 1 (the usual for depth-1 pixmaps).
    if (bitPlane != 1u) return;
    
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
      uint32_t* wPix = nullptr;
      uint32_t wW = 0, wH = 0;
      if (!x11_xproto_window_fb_rw(src, &wPix, &wW, &wH) || !wPix) return;
      srcPixels = wPix;
      srcW = (int)wW;
      srcH = (int)wH;
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
    //  - 32bpp pixels (window FB or depth>1 pixmap)
    // ------------------------------------------------------------
    uint32_t* dstPixels = nullptr;
    uint8_t*  dstBits   = nullptr;
    int dstW = 0, dstH = 0;
    uint32_t dstStrideBytes = 0;
    bool dstDepth1 = false;
    bool dstIsWindow = false;
    
    if (ctx.windows().exists(dst)) {
      uint32_t* wPix = nullptr;
      uint32_t wW = 0, wH = 0;
      if (!x11_xproto_window_fb_rw(dst, &wPix, &wW, &wH) || !wPix) return;
      dstPixels = wPix;
      dstW = (int)wW;
      dstH = (int)wH;
      dstDepth1 = false;
      dstIsWindow = true;
    } else {
      // try depth-1 bits first
      uint16_t pw = 0, ph = 0;
      uint32_t stride = 0;
      if (uint8_t* bits = ctx.pixmaps().mutableBits(dst, &pw, &ph, &stride)) {
        dstBits = bits;
        dstW = (int)pw;
        dstH = (int)ph;
        dstStrideBytes = stride;
        dstDepth1 = true;
        dstIsWindow = false;
      } else {
        uint16_t pw2 = 0, ph2 = 0;
        uint32_t* pix = ctx.pixmaps().mutablePixels(dst, &pw2, &ph2);
        if (!pix) return;
        dstPixels = pix;
        dstW = (int)pw2;
        dstH = (int)ph2;
        dstDepth1 = false;
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
          dstPixels[(size_t)dy * (size_t)dstW + (size_t)dx] = on ? fg : bg;
        }
      }
    }
    
    // Damage only if destination is a window.
    damageOrDirty(ctx, dst );
      
  }
  
  
  void DrawOps::handleClearArea(XProtoContext& ctx, uint16_t /*seq*/, uint8_t exposures, ByteReader& br)
  {
    // Body after 4-byte header (12 bytes):
    //   CARD32 window
    //   INT16  x
    //   INT16  y
    //   CARD16 width
    //   CARD16 height
    if (br.remaining() < 12) { br.skip(br.remaining()); return; }

    const uint32_t wid = br.readU32();
    const int16_t  x   = (int16_t)br.readU16();
    const int16_t  y   = (int16_t)br.readU16();
    const uint16_t wpx = br.readU16();
    const uint16_t hpx = br.readU16();
    br.skip(br.remaining());

    if (wid == 0) return;

    // ClearArea targets WINDOW only (bring-up).
    if (!ctx.windows().exists(wid)) return;

    // Get window framebuffer (C-side storage)
    uint32_t* pixels = nullptr;
    uint32_t fbW = 0, fbH = 0;
    if (!x11_xproto_window_fb_rw(wid, &pixels, &fbW, &fbH) || !pixels || fbW == 0 || fbH == 0) return;

    // X11 semantics: width/height == 0 means “to the right/bottom edge”
    int x0 = (int)x;
    int y0 = (int)y;

    int x1 = (wpx == 0) ? (int)fbW : (x0 + (int)wpx);
    int y1 = (hpx == 0) ? (int)fbH : (y0 + (int)hpx);

    // Clamp
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fbW) x1 = (int)fbW;
    if (y1 > (int)fbH) y1 = (int)fbH;

    if (x0 >= x1 || y0 >= y1) return;

    // Bring-up background: opaque white (matches your existing C behavior).
    const uint32_t bg = 0xFFFFFFFFu;

    for (int yy = y0; yy < y1; yy++) {
      uint32_t* row = pixels + (size_t)yy * (size_t)fbW;
      for (int xx = x0; xx < x1; xx++) {
        row[xx] = bg;
      }
    }

    // Damage -> present
    {
// xxx maybe temp      ctx.tracef("[DAMAGE_ROUTE] ClearArea sticks with wid=0x%08X \n", wid);
// xxx maybe temp      if (ctx.windows().isReadyToPresent(wid)) {
// xxx maybe temp         // IMPORTANT: damage must be attributed to the drawable that changed (wid),
// xxx maybe temp         // NOT the host. Swift will map wid -> host and choose source correctly.
// xxx maybe temp         x11_requests_push_damage(wid);
// xxx maybe temp       } else {
// xxx maybe temp         ctx.windows().markDirty(wid);
// xxx maybe temp       }
// xxx maybe temp ---- 
      if (ctx.windows().exists(wid)) {
        x11_requests_push_damage((wid));
      }
// xxx ---- maybe temp 
    }

    
    // Optional Expose event (only if client asked for exposures and selected ExposureMask)
    if (exposures) {
      if (const WindowView* vw = ctx.window(wid)) {
        const bool wantsExpose = vw->mapped && ((vw->event_mask & (1u << 15)) != 0);
        if (wantsExpose) {
          // Count=0 for bring-up (matches prior behavior)
          ctx.transport().queueExposeRect(wid,
                                         (uint16_t)x0, (uint16_t)y0,
                                         (uint16_t)(x1 - x0), (uint16_t)(y1 - y0),
                                         0);
        }
      }
    }
  }
  
  
} // namespace x11
