//
//  DrawOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "DrawOps.hpp"

#include <cstddef>
#include <cstdint>

#include "XProtoContext.hpp"
#include "ByteReader.hpp"
#include "PixmapTable.hpp"   // adjust include path to your project
#include "WindowTable.hpp"

namespace x11 {

static constexpr uint32_t kBitmapScanlinePadBits = 32;   // matches SetupSuccess
static constexpr bool     kBitmapBitOrderLSBFirst = true;

DrawOps::DrawOps(XProtoRegistrar& reg) {
  //reg.registerMajor(62, &DrawOps::onMajor, this); // CopyArea
  //reg.registerMajor(63, &DrawOps::onMajor, this); // CopyPlane
  reg.registerMajor(72, &DrawOps::onMajor, this); // PutImage
}

void DrawOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<DrawOps*>(user)->handle(ctx, dc);
}

void DrawOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case 72: handlePutImage(ctx, dc.seq, dc.minor /*format*/, dc.br); return;
    case 62: handleCopyArea(ctx, dc.seq, dc.br); return;
    case 63: handleCopyPlane(ctx, dc.seq, dc.br); return;
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

  // Destination: if drawable is a WINDOW, skip for now (we’ll add later).
  // xeyes uses pixmaps here.
  if (ctx.windows().exists(drawable)) {
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
    // truncated request body
    br.skip(br.remaining());
    return;
  }
  const std::size_t need = (std::size_t)need64;

  const uint8_t* src = br.ptr();   // raw packed bitmap data
  br.skip(br.remaining());         // consume rest (including request padding)

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

  // IMPORTANT: pixmaps are not presentable; do not enqueue damage here.
  (void)need;
}

// -----------------------------
// CopyArea (major 62) -- stub for now
// -----------------------------
void DrawOps::handleCopyArea(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  ctx.tracef("[DrawOps] CopyArea: stub (skipping %zu)\n", br.remaining());
  br.skip(br.remaining());
}

// -----------------------------
// CopyPlane (major 63) -- stub for now
// -----------------------------
void DrawOps::handleCopyPlane(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  ctx.tracef("[DrawOps] CopyPlane: stub (skipping %zu)\n", br.remaining());
  br.skip(br.remaining());
}

} // namespace x11
