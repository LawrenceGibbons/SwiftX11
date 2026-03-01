//
//  SelectionOps.hpp
//  X11LowLevel
//

#pragma once
#include <cstdint>
#include "Core/XProtoRegistrar.hpp"

namespace x11 {

class XProtoContext;
class ByteReader;

class SelectionOps {
public:
  explicit SelectionOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  void handleSetSelectionOwner(XProtoContext& ctx, uint16_t seq, ByteReader& br);  // 22
  void handleGetSelectionOwner(XProtoContext& ctx, uint16_t seq, ByteReader& br);  // 23 (reply)
  void handleConvertSelection(XProtoContext& ctx, uint16_t seq, ByteReader& br);   // 24
  void handleSendEvent(XProtoContext& ctx, uint16_t seq, uint8_t propagate, ByteReader& br); // 25
};

} // namespace x11
