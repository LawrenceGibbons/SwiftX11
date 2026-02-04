//
//  WireLE.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/4/26.
//

#pragma once
#include <cstdint>

namespace x11::wire {

// Always little-endian write (X11 protocol is LE/BE depending on client,
// but *your* server currently assumes little-endian clients only).
inline void wr16_le(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

inline void wr32_le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

inline uint16_t rd16_le(const uint8_t* p) {
  return (uint16_t)( (uint16_t)p[0] | ((uint16_t)p[1] << 8) );
}

inline uint32_t rd32_le(const uint8_t* p) {
  return (uint32_t)(
    (uint32_t)p[0] |
    ((uint32_t)p[1] << 8) |
    ((uint32_t)p[2] << 16) |
    ((uint32_t)p[3] << 24)
  );
}

} // namespace x11::wire
