//
//  DrawOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once

#include <cstdint>

#include "XProtoRegistrar.hpp"

namespace x11 {

class XProtoContext;
class ByteReader;

class DrawOps {
public:
  explicit DrawOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  // Major handlers
  void handlePutImage(XProtoContext& ctx, uint16_t seq, uint8_t format, ByteReader& br); // 72
  void handleCopyArea(XProtoContext& ctx, uint16_t seq, ByteReader& br);                  // 62 (stub)
  void handleCopyPlane(XProtoContext& ctx, uint16_t seq, ByteReader& br);                 // 63 (stub)

  // Helpers
  static uint32_t computeStrideBytesXY1(uint16_t width, uint8_t leftPadBits); // bitmapScanlinePad=32
};

} // namespace x11


extern "C" {
// Returns 1 on success, 0 on failure.
// outPixels points at the window framebuffer (32bpp), outW/outH are pixel dims.
int x11_xproto_window_fb_rw(uint32_t xid,
                            uint32_t** outPixels,
                            uint32_t* outW,
                            uint32_t* outH);

// Call the exact same damage gating as old C draw ops.
void x11_xproto_enqueue_damage(uint32_t xid);
  
} // extern "C"
