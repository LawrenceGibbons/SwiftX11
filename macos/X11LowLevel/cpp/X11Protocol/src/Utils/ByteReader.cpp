//
//  ByteReader.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/21/26.
//

#include "ByteReader.hpp"
#include <stdexcept>

namespace x11 {

static inline uint16_t rd16_le(const uint8_t* p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t rd32_le(const uint8_t* p) {
  return (uint32_t)(p[0] |
                   ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 24));
}

void ByteReader::skip(std::size_t n) {
  if (n > remaining()) throw std::runtime_error("ByteReader::skip past end");
  p_ += n;
}

uint8_t ByteReader::readU8() {
  if (remaining() < 1) throw std::runtime_error("ByteReader::readU8 past end");
  return *p_++;
}

uint16_t ByteReader::readU16() {
  if (remaining() < 2) throw std::runtime_error("ByteReader::readU16 past end");
  uint16_t v = rd16_le(p_);
  p_ += 2;
  return v;
}

uint32_t ByteReader::readU32() {
  if (remaining() < 4) throw std::runtime_error("ByteReader::readU32 past end");
  uint32_t v = rd32_le(p_);
  p_ += 4;
  return v;
}

} // namespace x11
