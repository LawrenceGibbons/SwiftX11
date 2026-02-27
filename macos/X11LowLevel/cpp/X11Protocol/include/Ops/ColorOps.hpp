//
//  ColorOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/10/26.
//

#pragma once
#include <cstdint>

#include "XProtoRegistrar.hpp"

namespace x11 {

class XProtoContext;
class ByteReader;

class ColorOps {
public:
  explicit ColorOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  // 78..90
  void handleCreateColormap(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br);         // 78 (no reply)
  void handleFreeColormap(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br);           // 79 (no reply)
  void handleCopyColormapAndFree(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br);    // 80 (no reply)
  void handleInstallColormap(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br);        // 81 (no reply)
  void handleUninstallColormap(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br);      // 82 (no reply)
  void handleListInstalledColormaps(XProtoContext& ctx, uint16_t seq, ByteReader& br);     // 83 (reply)
  void handleAllocColor(XProtoContext& ctx, uint16_t seq, ByteReader& br);                 // 84 (reply)
  void handleAllocNamedColor(XProtoContext& ctx, uint16_t seq, ByteReader& br);            // 85 (reply)
  void handleAllocColorCells(XProtoContext& ctx, uint16_t seq, ByteReader& br);            // 86 (reply)
  void handleAllocColorPlanes(XProtoContext& ctx, uint16_t seq, ByteReader& br);           // 87 (reply)
  void handleFreeColors(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br);             // 88 (no reply)
  void handleStoreColors(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br);            // 89 (no reply)
  void handleStoreNamedColor(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br);        // 90 (no reply)
  void handleLookupColor(XProtoContext& ctx, uint16_t seq, ByteReader& br);                // 92 (reply)
};

} // namespace x11
