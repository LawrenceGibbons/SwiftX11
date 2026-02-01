//
//  QueryOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once

#include <cstdint>

#include "XProtoRegistrar.hpp"

namespace x11 {

class QueryOps {
public:
  explicit QueryOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);

  // Handles core “query” requests by major opcode:
  // 14 GetGeometry
  // 15 QueryTree
  // 38 QueryPointer
  // 43 GetInputFocus
  // 91 QueryColors
  void handle(XProtoContext& ctx, DispatchContext& dc);


private:
  // Per-op handlers (these are your C ports)
  void handleQueryTree(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  void handleQueryPointer(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  void handleGetInputFocus(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  void handleQueryColors(XProtoContext& ctx, uint16_t seq, ByteReader& br);

private:
  // Fake pointer state (bring-up)
  int16_t fake_rx_ = 0;
  int16_t fake_ry_ = 0;
};

} // namespace x11
