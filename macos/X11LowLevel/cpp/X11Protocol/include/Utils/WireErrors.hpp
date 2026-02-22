//
//  WireErrors.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/22/26.
//

// Utils/WireErrors.hpp
#pragma once
#include <array>
#include <cstdint>
#include "Utils/WireLE.hpp"   // wire::wr16_le / wr32_le

namespace x11::wireerr {

// X11 xError (32 bytes):
// [0]=0, [1]=errorCode, [2..3]=seq, [4..7]=resourceId, [8..9]=minorCode,
// [10]=majorCode, [11]=pad, rest zeros.
inline std::array<uint8_t, 32> buildError32(uint8_t errorCode,
                                            uint16_t seq,
                                            uint32_t resourceId,
                                            uint16_t minorCode,
                                            uint8_t majorCode)
{
  std::array<uint8_t, 32> e{};
  e.fill(0);
  e[0] = 0;
  e[1] = errorCode;
  wire::wr16_le(e.data() + 2, seq);
  wire::wr32_le(e.data() + 4, resourceId);
  wire::wr16_le(e.data() + 8, minorCode);
  e[10] = majorCode;
  e[11] = 0;
  return e;
}

// Convenience for core requests where minorCode is usually 0.
inline std::array<uint8_t, 32> buildCoreError32(uint8_t errorCode,
                                                uint16_t seq,
                                                uint32_t resourceId,
                                                uint8_t majorCode)
{
  return buildError32(errorCode, seq, resourceId, /*minorCode=*/0, majorCode);
}

} // namespace x11::wireerr
