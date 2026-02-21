//
//  RasterOp.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/21/26.
//

#pragma once
#include <cstdint>

// X11 GX* functions are defined on “pixel values” (no alpha concept).
// Your pipeline uses ARGB with alpha=0xFF for display.
// IMPORTANT: keep alpha forced to 0xFF so GXinvert doesn’t make pixels transparent.

static inline uint32_t x11_rop24(uint32_t d24, uint32_t s24, uint8_t fn)
{
  switch (fn & 0x0Fu) {
    case 0:  return 0x000000u;                 // GXclear
    case 1:  return (s24 & d24);               // GXand
    case 2:  return (s24 & ~d24);              // GXandReverse
    case 3:  return s24;                        // GXcopy
    case 4:  return (~s24 & d24);              // GXandInverted
    case 5:  return d24;                        // GXnoop
    case 6:  return (s24 ^ d24);               // GXxor
    case 7:  return (s24 | d24);               // GXor
    case 8:  return ~(s24 | d24) & 0x00FFFFFFu;// GXnor
    case 9:  return ~(s24 ^ d24) & 0x00FFFFFFu;// GXequiv
    case 10: return (~d24) & 0x00FFFFFFu;      // GXinvert
    case 11: return (s24 | ~d24) & 0x00FFFFFFu;// GXorReverse
    case 12: return (~s24) & 0x00FFFFFFu;      // GXcopyInverted
    case 13: return ((~s24) | d24) & 0x00FFFFFFu; // GXorInverted
    case 14: return ~(s24 & d24) & 0x00FFFFFFu;// GXnand
    case 15: return 0x00FFFFFFu;               // GXset
    default: return s24;
  }
}

// Apply GX* + plane mask to a destination ARGB pixel.
// We treat pixels as XRGB (24-bit) and force A=0xFF on output.
static inline uint32_t x11_apply_rop_argb(uint32_t dst_argb,
                                         uint32_t src_argb,
                                         uint8_t  fn,
                                         uint32_t plane_mask)
{
  const uint32_t d = dst_argb & 0x00FFFFFFu;
  const uint32_t s = src_argb & 0x00FFFFFFu;

  const uint32_t r = x11_rop24(d, s, fn);

  const uint32_t pm = plane_mask & 0x00FFFFFFu;
  const uint32_t out = (d & ~pm) | (r & pm);

  return 0xFF000000u | out;
}
