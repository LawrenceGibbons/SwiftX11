//
//  QueryOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once

#include <cstdint>

#include <Core/XProtoRegistrar.hpp>

namespace x11 {

class QueryOps {
public:
  explicit QueryOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);

  // Handles core “query” requests by major opcode:
  //  14 GetGeometry
  //  15 QueryTree
  //  38 QueryPointer
  //  43 GetInputFocus
  //  91 QueryColors
  //  98 QueryExtension
  //  99 ListExtensions
  // 101 GetKeyboardMapping
  // 119 GetModifierMapping
  void handle(XProtoContext& ctx, DispatchContext& dc);


private:
  // Per-op handlers (these are your C ports)
  void handleQueryTree(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  void handleQueryPointer(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  void handleGetInputFocus(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  void handleQueryColors(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  void handleQueryExtension(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  void handleListExtensions(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  void handleGetKeyboardMapping(XProtoContext& ctx, uint16_t seq, ByteReader& br); // 101
  void handleTranslateCoords(XProtoContext& ctx, uint16_t seq, ByteReader& br); // 40
  void handleWarpPointer(XProtoContext& ctx, uint16_t seq, ByteReader& br); // 41
  void handleSetInputFocus(XProtoContext& ctx, uint16_t seq, uint8_t revertTo, ByteReader& br); // 42
  void handleGetMotionEvents(XProtoContext& ctx, uint16_t seq, ByteReader& br); // 39
  void handleQueryKeymap(XProtoContext& ctx, uint16_t seq, ByteReader& br); // 44
};

} // namespace x11
