//
//  PixmapOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/29/26.
//

//
//  PixmapOps.hpp
//  X11LowLevel
//

#pragma once

#include <cstdint>
#include <Core/XProtoRegistrar.hpp>

namespace x11 {

class PixmapOps {
public:
  explicit PixmapOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  // major 53: CreatePixmap (depth is dc.minor)
  void handleCreatePixmap(XProtoContext& ctx, uint8_t depth, ByteReader& br);

  // major 54: FreePixmap
  void handleFreePixmap(XProtoContext& ctx, ByteReader& br);
};

} // namespace x11
