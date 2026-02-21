//
//  WireEvents.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/19/26.
//

// Utils/WireEvents.hpp
#pragma once
#include <array>
#include <cstdint>
#include "Utils/WireLE.hpp"

namespace x11::wireev {

// Expose (type 12)
inline std::array<uint8_t,32> buildExpose(uint16_t seq,
                                         uint32_t window,
                                         uint16_t x, uint16_t y,
                                         uint16_t w, uint16_t h,
                                         uint16_t count)
{
  std::array<uint8_t,32> ev{};
  ev.fill(0);
  ev[0] = 12; // Expose
  wire::wr16_le(ev.data() + 2, seq);
  wire::wr32_le(ev.data() + 4, window);
  wire::wr16_le(ev.data() + 8,  x);
  wire::wr16_le(ev.data() + 10, y);
  wire::wr16_le(ev.data() + 12, w);
  wire::wr16_le(ev.data() + 14, h);
  wire::wr16_le(ev.data() + 16, count);
  return ev;
}

// NoExpose (type 14) — used to complete CopyArea/CopyPlane when graphics_exposures is on
inline std::array<uint8_t,32> buildNoExpose(uint16_t seq,
                                            uint32_t drawable,
                                            uint8_t majorEvent,
                                            uint16_t minorEvent)
{
  std::array<uint8_t,32> ev{};
  ev.fill(0);
  ev[0] = 14; // NoExpose
  wire::wr16_le(ev.data() + 2, seq);
  wire::wr32_le(ev.data() + 4, drawable);
  wire::wr16_le(ev.data() + 8, minorEvent);
  ev[10] = majorEvent;
  return ev;
}

} // namespace x11::wireev
