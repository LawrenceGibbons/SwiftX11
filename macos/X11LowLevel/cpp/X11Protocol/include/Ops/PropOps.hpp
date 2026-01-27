//
//  PropOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

// PropOps.hpp
#pragma once

#include <cstdint>
#include <cstddef>

#include "XProtoRegistrar.hpp"

namespace x11 {

class XProtoContext;
class ByteReader;

class PropOps {
public:
  explicit PropOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  void handleChangeProperty(XProtoContext& ctx, uint16_t seq, uint8_t mode, ByteReader& br);
  void handleGetProperty(XProtoContext& ctx, uint16_t seq, uint8_t deleteFlag, ByteReader& br);
};

} // namespace x11
