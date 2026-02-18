//
//  GrabOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/17/26.
//

#pragma once
#include "Core/XProtoRegistrar.hpp"

namespace x11 {

class XProtoContext;
class ByteReader;

class GrabOps {
public:
  explicit GrabOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  void handleGrabPointer(XProtoContext& ctx, uint16_t seq, uint8_t ownerEvents, ByteReader& br);   // 26
  void handleUngrabPointer(XProtoContext& ctx, uint16_t seq, ByteReader& br);                      // 27
  void handleGrabButton (XProtoContext& ctx, uint16_t seq, uint8_t ownerEvents, ByteReader& br);   // 28
  void handleUngrabButton(XProtoContext& ctx, uint16_t seq, uint8_t button, ByteReader& br);       // 29
  
};

} // namespace x11
