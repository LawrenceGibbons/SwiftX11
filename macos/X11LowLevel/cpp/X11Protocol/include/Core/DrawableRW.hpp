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
// - pixels32 : 32bpp ARGB framebuffer
// - bits1    : packed 1bpp bitmap (LSBFirst)
//
// If is_window == true, caller must enqueue damage after modification.
  
  
class XProtoContext;
  
struct DrawableRW {
  bool is_window = false;

  uint16_t w = 0;
  uint16_t h = 0;

  // 32bpp
  uint32_t* pixels32 = nullptr;

  // 1bpp
  uint8_t*  bits1 = nullptr;
  uint32_t  stride_bytes = 0;
};

  
  
  bool resolveDrawableRW(XProtoContext& ctx,
                         uint32_t drawable,
                         DrawableRW& out);

  
  
} // namespace x11

extern "C" int x11_xproto_window_fb_rw(uint32_t xid,
                                      uint32_t** outPixels,
                                      uint32_t* outW,
                                      uint32_t* outH);

