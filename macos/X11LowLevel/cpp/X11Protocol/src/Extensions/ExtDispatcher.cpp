//
//  ExtDispatcher.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstddef>
#include <cstdio>

#include "ExtDispatcher.hpp"
#include "Utils/ByteReader.hpp"

extern "C" {
#include "SwiftX11Bridge.h"
}

namespace x11 {

void ExtDispatcher::handle(uint8_t majorOpcode, uint8_t minorOpcode, ByteReader& br) {
  // Stub: swallow payload for now.
  char buf[128];
  snprintf(buf, sizeof(buf),
           "[ExtDispatcher] unhandled extension opcode major=%u minor=%u (stub) skip=%zu\n",
           (unsigned)majorOpcode, (unsigned)minorOpcode, br.remaining());
  x11_ui_push_log(1, buf);
  br.skip(br.remaining());
}

} // namespace x11
