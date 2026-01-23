//
//  XInput2Ops.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once
#include <cstdint>

namespace x11 {

class XProtoContext;
class ByteReader;

class XInput2Ops {
public:
  explicit XInput2Ops(XProtoContext& ctx) : ctx_(ctx) {}
  void handle(uint8_t minorOpcode, ByteReader& br);

private:
  XProtoContext& ctx_;
};

} // namespace x11
