//
//  DrawOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once

#include <cstdint>

#include <Core/XProtoRegistrar.hpp>

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
  void handleClearArea(XProtoContext& ctx, uint16_t seq, uint8_t exposures, ByteReader& br); // 61
  void handleCopyArea(XProtoContext& ctx, uint16_t seq, ByteReader& br);                     // 62
  void handleCopyPlane(XProtoContext& ctx, uint16_t seq, ByteReader& br);                    // 63
  void handlePutImage(XProtoContext& ctx, uint16_t seq, uint8_t format, ByteReader& br);     // 72
  void handleGetImage(XProtoContext& ctx, uint16_t seq, uint8_t format, ByteReader& br);    // 73
  void handlePolyText8(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br);                // 74
  void handleImageText8(XProtoContext& ctx, uint16_t /*seq*/, uint8_t n, ByteReader& br);    // 76
  // Helpers
  static uint32_t computeStrideBytesXY1(uint16_t width, uint8_t leftPadBits); // bitmapScanlinePad=32
};

} // namespace x11

  
