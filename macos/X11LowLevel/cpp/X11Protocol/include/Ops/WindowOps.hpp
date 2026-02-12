//  WindowOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#pragma once

#include <cstdint>
#include <Core/XProtoRegistrar.hpp>

namespace x11 {

class XProtoContext;
class ByteReader;

class WindowOps {
public:
  explicit WindowOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  // ---- Core window ops ----
  void handleCreateWindow(XProtoContext& ctx, uint16_t seq, uint8_t depth, ByteReader& br);         // major 1
  void handleDestroyWindow(XProtoContext& ctx, uint16_t seq, ByteReader& br);                       // major 4
  
  // Major 8: MapWindow (you already did)  
  void handleMapWindow(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  void handleMapSubwindows(XProtoContext& ctx, uint16_t seq, ByteReader& br);                       // major 9
  
  // Major 10: UnmapWindow 
  void handleUnmapWindow(XProtoContext& ctx, uint16_t seq, ByteReader& br);
  
  
  void handleConfigureWindow(XProtoContext& ctx, uint16_t seq, ByteReader& br);                     // major 12
};

} // namespace x11
