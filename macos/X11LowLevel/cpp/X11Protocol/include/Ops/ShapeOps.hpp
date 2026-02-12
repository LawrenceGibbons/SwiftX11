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
//   68 PolyArc
//   70 PolyFillRectangle (stub for now)
//   71 PolyFillArc
class ShapeOps {
public:
  explicit ShapeOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  void handlePolyArc(XProtoContext& ctx, uint16_t seq, ByteReader& br);        // 68
  void handlePolyFillRectangle(XProtoContext& ctx, uint16_t seq, ByteReader& br); // 70 (stub)
  void handlePolyFillArc(XProtoContext& ctx, uint16_t seq, ByteReader& br);    // 71
};

} // namespace x11
