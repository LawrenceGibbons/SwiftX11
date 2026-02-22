//
//  SurfaceDesc.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/22/26.
//

#pragma once
#include <cstdint>

namespace x11 {

enum class SurfaceFormat : uint32_t {
  // Pick what you want. Start with what you already effectively use: 32bpp XRGB with masks 00FF0000/0000FF00/000000FF.
  XRGB8888 = 1,
};

struct SurfaceDesc {
  void*    ptr = nullptr;       // CPU-writable base pointer
  uint32_t bytesPerRow = 0;     // stride in bytes
  uint16_t w = 0;
  uint16_t h = 0;
  SurfaceFormat format = SurfaceFormat::XRGB8888;
  uint32_t generation = 0;      // bump on realloc; helps debugging
};

} // namespace x11
