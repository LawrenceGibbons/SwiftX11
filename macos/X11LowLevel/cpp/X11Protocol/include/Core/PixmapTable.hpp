//
//  PixmapTable.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/28/26.
//

#pragma once

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace x11 {

struct PixmapView {
  uint32_t xid = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  uint8_t  depth = 0;

  // depth==1
  uint32_t stride_bytes = 0;
  const uint8_t* bits = nullptr;

  // depth!=1
  const uint32_t* pixels = nullptr;
};

class PixmapTable {
public:
  PixmapTable() = default;

  // major=53 CreatePixmap
  // Initializes storage to 0 (depth==1) or white (depth!=1).
  void createPixmap(uint32_t xid, uint8_t depth, uint16_t w, uint16_t h);

  // major=54 FreePixmap
  void freePixmap(uint32_t xid);

  // Read-only snapshot. Pointers remain valid until next mutation of same pixmap.
  bool snapshot(uint32_t xid, PixmapView& out) const;

  // Writable accessors for DrawOps
  // Returns nullptr if not found or wrong depth type.
  uint32_t* mutablePixels(uint32_t xid, uint16_t* outW, uint16_t* outH);
  uint8_t*  mutableBits(uint32_t xid, uint16_t* outW, uint16_t* outH, uint32_t* outStrideBytes);

  // Helpers
  bool exists(uint32_t xid) const;

private:
  struct PixmapState {
    uint32_t xid = 0;
    uint16_t w = 1;
    uint16_t h = 1;
    uint8_t  depth = 0;

    // depth==1 packed bits
    uint32_t stride_bytes = 0;
    std::vector<uint8_t> bits;

    // depth!=1 32bpp pixels
    std::vector<uint32_t> pixels;
  };

  mutable std::mutex mu_;
  std::unordered_map<uint32_t, PixmapState> map_;

  static uint32_t computeStrideBytesDepth1(uint16_t w);
};

} // namespace x11
