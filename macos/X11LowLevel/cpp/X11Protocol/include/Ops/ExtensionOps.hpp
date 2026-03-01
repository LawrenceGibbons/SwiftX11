//
//  ExtensionOps.hpp
//  X11LowLevel
//
//  Extension stub handlers (XFIXES, SHAPE, RANDR, Xinerama, Generic Events, etc.)
//

#pragma once

#include <cstdint>
#include <Core/XProtoRegistrar.hpp>

namespace x11 {

class XProtoContext;
class ByteReader;

class ExtensionOps {
public:
  explicit ExtensionOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);
};

} // namespace x11
