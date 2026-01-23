//
//  CompositeOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once
#include <cstdint>

namespace x11 {

class XProtoContext;
class ByteReader;

class CompositeOps {
public:
  explicit CompositeOps(XProtoContext& ctx) : ctx_(ctx) {}
  void handle(uint8_t minorOpcode, ByteReader& br);

private:
  XProtoContext& ctx_;
};

} // namespace x11
