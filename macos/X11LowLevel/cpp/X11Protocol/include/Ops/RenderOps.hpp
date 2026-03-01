//
//  RenderOps.hpp
//  X11LowLevel
//
//  RENDER extension (X11 compositing, anti-aliased text, alpha blending).
//  Handles the extension's major opcode; sub-opcodes dispatched via dc.minor.
//

#pragma once
#include <cstdint>
#include <Core/XProtoRegistrar.hpp>

namespace x11 {

class XProtoContext;
class ByteReader;

class RenderOps {
public:
  explicit RenderOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);
};

} // namespace x11
