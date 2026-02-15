//
//  ByteReader.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/4/26.
//
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

namespace x11 {

class ByteReader {
public:
  ByteReader(const uint8_t* data, std::size_t len)
    : p_(data), begin_(data), end_(data + len) {}

  // ------------------------------------------------------------
  // Basic state
  // ------------------------------------------------------------

  std::size_t remaining() const {
    return (p_ <= end_) ? (std::size_t)(end_ - p_) : 0;
  }

  std::size_t consumed() const {
    return (std::size_t)(p_ - begin_);
  }

  const uint8_t* ptr() const { return p_; }

  void skip(std::size_t n) {
    if (remaining() < n) throw std::runtime_error("ByteReader: skip overflow");
    p_ += n;
  }

  void align4() {
    std::size_t pad = (4 - (consumed() & 3)) & 3;
    if (pad) skip(pad);
  }

  void rewind() { p_ = begin_; }

  // ------------------------------------------------------------
  // Raw byte access
  // ------------------------------------------------------------

  const uint8_t* readBytes(std::size_t n) {
    if (remaining() < n) throw std::runtime_error("ByteReader: readBytes overflow");
    const uint8_t* out = p_;
    p_ += n;
    return out;
  }

  const uint8_t* peekBytes(std::size_t n) const {
    if (remaining() < n) return nullptr;
    return p_;
  }

  std::vector<uint8_t> readVector(std::size_t n) {
    const uint8_t* src = readBytes(n);
    return std::vector<uint8_t>(src, src + n);
  }

  std::string readString(std::size_t n) {
    const uint8_t* src = readBytes(n);
    return std::string(reinterpret_cast<const char*>(src), n);
  }

  // ------------------------------------------------------------
  // Unsigned reads (Little Endian — X11 wire default)
  // ------------------------------------------------------------

  uint8_t readU8() {
    if (remaining() < 1) throw std::runtime_error("ByteReader: readU8 overflow");
    return *p_++;
  }

  uint16_t readU16() {
    if (remaining() < 2) throw std::runtime_error("ByteReader: readU16 overflow");
    uint16_t v = (uint16_t)p_[0] |
                 ((uint16_t)p_[1] << 8);
    p_ += 2;
    return v;
  }

  uint32_t readU32() {
    if (remaining() < 4) throw std::runtime_error("ByteReader: readU32 overflow");
    uint32_t v =  (uint32_t)p_[0] |
                 ((uint32_t)p_[1] << 8) |
                 ((uint32_t)p_[2] << 16) |
                 ((uint32_t)p_[3] << 24);
    p_ += 4;
    return v;
  }

  // ------------------------------------------------------------
  // Signed reads (Little Endian)
  // ------------------------------------------------------------

  int8_t readI8() {
    return (int8_t)readU8();
  }

  int16_t readI16() {
    return (int16_t)readU16();
  }

  int32_t readI32() {
    return (int32_t)readU32();
  }

  // ------------------------------------------------------------
  // Big Endian variants (needed if client byte order differs)
  // ------------------------------------------------------------

  uint16_t readU16BE() {
    if (remaining() < 2) throw std::runtime_error("ByteReader: readU16BE overflow");
    uint16_t v = ((uint16_t)p_[0] << 8) |
                  (uint16_t)p_[1];
    p_ += 2;
    return v;
  }

  uint32_t readU32BE() {
    if (remaining() < 4) throw std::runtime_error("ByteReader: readU32BE overflow");
    uint32_t v = ((uint32_t)p_[0] << 24) |
                 ((uint32_t)p_[1] << 16) |
                 ((uint32_t)p_[2] << 8)  |
                  (uint32_t)p_[3];
    p_ += 4;
    return v;
  }

  int16_t readI16BE() {
    return (int16_t)readU16BE();
  }

  int32_t readI32BE() {
    return (int32_t)readU32BE();
  }

  // ------------------------------------------------------------
  // Peek variants (non-advancing)
  // ------------------------------------------------------------

  uint8_t peekU8() const {
    if (remaining() < 1) throw std::runtime_error("ByteReader: peekU8 overflow");
    return *p_;
  }

  uint16_t peekU16() const {
    if (remaining() < 2) throw std::runtime_error("ByteReader: peekU16 overflow");
    return (uint16_t)p_[0] |
           ((uint16_t)p_[1] << 8);
  }

  uint32_t peekU32() const {
    if (remaining() < 4) throw std::runtime_error("ByteReader: peekU32 overflow");
    return  (uint32_t)p_[0] |
            ((uint32_t)p_[1] << 8) |
            ((uint32_t)p_[2] << 16) |
            ((uint32_t)p_[3] << 24);
  }

  // ------------------------------------------------------------
  // Safe try-read versions (no exceptions)
  // ------------------------------------------------------------

  bool tryReadU32(uint32_t& out) {
    if (remaining() < 4) return false;
    out = readU32();
    return true;
  }

  bool tryReadU16(uint16_t& out) {
    if (remaining() < 2) return false;
    out = readU16();
    return true;
  }

private:
  const uint8_t* p_ = nullptr;
  const uint8_t* begin_ = nullptr;
  const uint8_t* end_ = nullptr;
};

} // namespace x11
