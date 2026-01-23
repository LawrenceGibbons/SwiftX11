//
//  ExtDispatcher.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstddef>

#include "ExtDispatcher.hpp"

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

void ExtDispatcher::handle(uint8_t majorOpcode, uint8_t minorOpcode, ByteReader& br) {
  // Stub: swallow payload for now.
  ctx_.tracef("[ExtDispatcher] unhandled extension opcode major=%u minor=%u (stub) skip=%zu\n",
              (unsigned)majorOpcode, (unsigned)minorOpcode, br.remaining());
  br.skip(br.remaining());
}

} // namespace x11
