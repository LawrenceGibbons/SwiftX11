//
//  WindowAttrOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/26/26.
//

#pragma once
#include <cstdint>
#include <Core/XProtoRegistrar.hpp>

namespace x11 {

class XProtoContext;
class ByteReader;

class WindowAttrOps {
public:
  explicit WindowAttrOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  // Major  2: ChangeWindowAttributes
  void handleChangeWindowAttributes(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  
  // Major 3: GetWindowAttributes
  void handleGetWindowAttributes(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  
  // Major 12: ConfigureWindow
  void handleConfigureWindow(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  
  // Major 14: GetGeometry
  void handleGetGeometry(XProtoContext& ctx, uint16_t seq, ByteReader& br);

};

} // namespace x11

