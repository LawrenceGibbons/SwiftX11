//
//  DrawableRW.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/30/26.
//

#pragma once

#include <cstdint>

namespace x11 {

  // Unified writable drawable view used by DrawOps / ShapeOps / future ops.
  //
  // Exactly one of (pixels32, bits1) is non-null.
  // - pixels32 : 32bpp little-endian pixels (BGRA/XRGB depending on your pipeline; alpha is treated as opaque).
  // - bits1    : packed 1bpp bitmap (LSBFirst).
  //
  // NOTE: pixels32 may be *strided* (bytesPerRow alignment). Always use stridePixels (or bytesPerRow)
  // when stepping rows; do NOT assume row stride == w.
  //
  // If is_window == true, caller must mark dirty / enqueue damage after modification (typically on the host).
  
struct DrawableRW {
  uint32_t* pixels32 = nullptr;

  uint16_t w = 0;
  uint16_t h = 0;

  // NEW: stride in pixels (row-to-row step), not necessarily == w.
  uint32_t stridePixels = 0;

  bool isWindow = false;
  bool isPixmap = false;

  uint8_t depth = 32;
};

class XProtoContext;

// Returns true if drawable is a WINDOW or PIXMAP with writable 32bpp pixels.
bool resolveDrawableRW(XProtoContext& ctx, uint32_t drawable, DrawableRW& out);

} // namespace x11

extern "C" int x11_xproto_window_fb_rw(uint32_t xid,
                                      uint32_t** outPixels,
                                      uint32_t* outW,
                                      uint32_t* outH);

