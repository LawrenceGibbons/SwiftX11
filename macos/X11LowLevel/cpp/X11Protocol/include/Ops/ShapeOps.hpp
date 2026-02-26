//
//  ShapeOps.hpp
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

// Shape / raster ops:
//   64 PolyPoint
//   65 PolyLine
//   66 PolySegment
//   67 PolyRectangle
//   68 PolyArc
//   70 PolyFillRectangle
//   71 PolyFillArc
class ShapeOps {
public:
  explicit ShapeOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  void handlePolyPoint(XProtoContext& ctx, uint16_t seq, uint8_t coordMode, ByteReader& br); // 64
  void handlePolyLine(XProtoContext& ctx, uint16_t seq, uint8_t coordMode, ByteReader& br);  // 65
  void handlePolySegment(XProtoContext& ctx, uint16_t seq, ByteReader& br);    // 66
  void handlePolyRectangle(XProtoContext& ctx, uint16_t seq, ByteReader& br);  // 67
  void handlePolyArc(XProtoContext& ctx, uint16_t seq, ByteReader& br);        // 68
  void handlePolyFillRectangle(XProtoContext& ctx, uint16_t seq, ByteReader& br); // 70
  void handlePolyFillArc(XProtoContext& ctx, uint16_t seq, ByteReader& br);    // 71
};

} // namespace x11
