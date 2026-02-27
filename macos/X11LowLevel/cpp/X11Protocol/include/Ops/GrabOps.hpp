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

  void handleGrabKeyboard(XProtoContext& ctx, uint16_t seq, uint8_t ownerEvents, ByteReader& br);  // 31
  void handleUngrabKeyboard(XProtoContext& ctx, uint16_t seq, ByteReader& br);                     // 32
  void handleGrabKey(XProtoContext& ctx, uint16_t seq, uint8_t ownerEvents, ByteReader& br);       // 33
  void handleUngrabKey(XProtoContext& ctx, uint16_t seq, uint8_t keycode, ByteReader& br);         // 34
  void handleAllowEvents(XProtoContext& ctx, uint16_t seq, uint8_t mode, ByteReader& br);          // 35
  void handleGrabServer(XProtoContext& ctx, uint16_t seq, ByteReader& br);                         // 36
  void handleUngrabServer(XProtoContext& ctx, uint16_t seq, ByteReader& br);                       // 37
};

} // namespace x11
