//
//  WindowAttrOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/26/26.
//

#pragma once
#include <cstdint>
#include "XProtoRegistrar.hpp"

namespace x11 {

class XProtoContext;
class ByteReader;

class WindowAttrOps {
public:
  explicit WindowAttrOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  void handleChangeWindowAttributes(XProtoContext& ctx, uint16_t seq, ByteReader& br);
};

} // namespace x11

