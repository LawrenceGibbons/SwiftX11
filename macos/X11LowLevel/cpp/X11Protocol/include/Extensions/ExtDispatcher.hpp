//
//  ExtDispatcher.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once
#include <cstdint>

namespace x11 {

class XProtoContext;
class ByteReader;

// Extension dispatcher stub.
// Later: this will map “extension major opcodes” to a specific extension module
// (XRender, RandR, XI2, etc.) based on QueryExtension registration results.
class ExtDispatcher {
public:
  explicit ExtDispatcher(XProtoContext& ctx) : ctx_(ctx) {}

  // Called when major opcode >= 128 (extension range) OR when an extension is otherwise routed here.
  void handle(uint8_t majorOpcode, uint8_t minorOpcode, ByteReader& br);

private:
  XProtoContext& ctx_;
};

} // namespace x11
