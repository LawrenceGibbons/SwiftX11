//
//  PointerOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/18/26.
//

#pragma once

#include <array>
#include <cstdint>

#include "XProtoRegistrar.hpp"

namespace x11 {

class XProtoContext;
class ByteReader;
struct DispatchContext;

class PointerOps {
public:
  explicit PointerOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  void handleGetPointerMapping(XProtoContext& ctx, uint16_t seq);
  void handleSetPointerMapping(XProtoContext& ctx, uint16_t seq, uint8_t nElts, ByteReader& br);

  // Server-global pointer map. Default: 7 buttons.
  static std::array<uint8_t, 32> s_map;
  static uint8_t s_n;
};

} // namespace x11
