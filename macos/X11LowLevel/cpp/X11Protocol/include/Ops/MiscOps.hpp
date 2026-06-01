//
//  MiscOps.hpp
//  X11LowLevel
//

#pragma once
#include <cstdint>
#include <Core/XProtoRegistrar.hpp>

namespace x11 {

class XProtoContext;
class ByteReader;

// Misc / server-control ops:
//   100 ChangeKeyboardMapping
//   102 ChangeKeyboardControl
//   103 GetKeyboardControl      (reply)
//   104 Bell
//   105 ChangePointerControl
//   106 GetPointerControl       (reply)
//   107 SetScreenSaver
//   108 GetScreenSaver          (reply)
//   109 ChangeHosts
//   110 ListHosts               (reply)
//   111 SetAccessControl
//   112 SetCloseDownMode
//   113 KillClient
//   115 ForceScreenSaver
//   127 NoOperation
class MiscOps {
public:
  explicit MiscOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  void handleChangeKeyboardMapping(XProtoContext& ctx, uint16_t seq, ByteReader& br);  // 100
  void handleChangeKeyboardControl(XProtoContext& ctx, uint16_t seq, ByteReader& br);  // 102
  void handleGetKeyboardControl(XProtoContext& ctx, uint16_t seq, ByteReader& br);     // 103 (reply)
  void handleBell(XProtoContext& ctx, uint16_t seq, ByteReader& br);                   // 104
  void handleChangePointerControl(XProtoContext& ctx, uint16_t seq, ByteReader& br);   // 105
  void handleGetPointerControl(XProtoContext& ctx, uint16_t seq, ByteReader& br);      // 106 (reply)
  void handleSetScreenSaver(XProtoContext& ctx, uint16_t seq, ByteReader& br);         // 107
  void handleGetScreenSaver(XProtoContext& ctx, uint16_t seq, ByteReader& br);         // 108 (reply)
  void handleChangeHosts(XProtoContext& ctx, uint16_t seq, ByteReader& br);            // 109
  void handleListHosts(XProtoContext& ctx, uint16_t seq, ByteReader& br);              // 110 (reply)
  void handleSetAccessControl(XProtoContext& ctx, uint16_t seq, ByteReader& br);       // 111
  void handleSetCloseDownMode(XProtoContext& ctx, uint16_t seq, uint8_t mode,
                              ByteReader& br);                                          // 112
  void handleKillClient(XProtoContext& ctx, uint16_t seq, ByteReader& br);             // 113
  void handleForceScreenSaver(XProtoContext& ctx, uint16_t seq, ByteReader& br);       // 115
  void handleNoOperation(XProtoContext& ctx, uint16_t seq, ByteReader& br);            // 127
};

} // namespace x11
