#pragma once
#include <cstddef>
#include <cstdint>

namespace x11 {

class ByteReader {
public:
  ByteReader(const uint8_t* data, std::size_t len) : p_(data), end_(data + len) {}

  std::size_t remaining() const { return (p_ <= end_) ? (std::size_t)(end_ - p_) : 0; }

  void skip(std::size_t n);

  uint8_t  readU8();
  uint16_t readU16();
  uint32_t readU32();

  const uint8_t* ptr() const { return p_; }

private:
  const uint8_t* p_ = nullptr;
  const uint8_t* end_ = nullptr;
};

} // namespace x11
