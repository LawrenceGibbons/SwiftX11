//
//  ShmOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstddef>
#include <cstdio>

#include "ShmOps.hpp"

extern "C" {
#include "SwiftX11Bridge.h"
}

namespace x11 {

class ByteReader {
public:
  std::size_t remaining() const;
  void skip(size_t n);
};

class XProtoContext {
public:
  void tracef(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};

void ShmOps::handle(uint8_t minorOpcode, ByteReader& br) {
  char buf[128];
  snprintf(buf, sizeof(buf), "[ShmOps] unhandled minor=%u (stub) skip=%zu\n",
           (unsigned)minorOpcode, br.remaining());
  x11_ui_push_log(1, buf);
  br.skip(br.remaining());
}

} // namespace x11
