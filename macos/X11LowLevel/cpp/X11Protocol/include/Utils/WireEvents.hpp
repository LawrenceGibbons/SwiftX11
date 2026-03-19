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

// CreateNotify (type 16)
inline std::array<uint8_t,32> buildCreateNotify(uint16_t seq,
                                                uint32_t parent,
                                                uint32_t window,
                                                int16_t x, int16_t y,
                                                uint16_t w, uint16_t h,
                                                uint16_t borderWidth,
                                                bool overrideRedirect)
{
  std::array<uint8_t,32> ev{};
  ev.fill(0);
  ev[0] = 16; // CreateNotify
  wire::wr16_le(ev.data() + 2, seq);
  wire::wr32_le(ev.data() + 4, parent);          // event (parent)
  wire::wr32_le(ev.data() + 8, window);           // window
  wire::wr16_le(ev.data() + 12, (uint16_t)x);
  wire::wr16_le(ev.data() + 14, (uint16_t)y);
  wire::wr16_le(ev.data() + 16, w);
  wire::wr16_le(ev.data() + 18, h);
  wire::wr16_le(ev.data() + 20, borderWidth);
  ev[22] = overrideRedirect ? 1 : 0;
  return ev;
}

// DestroyNotify (type 17)
inline std::array<uint8_t,32> buildDestroyNotify(uint16_t seq,
                                                 uint32_t event,
                                                 uint32_t window)
{
  std::array<uint8_t,32> ev{};
  ev.fill(0);
  ev[0] = 17; // DestroyNotify
  wire::wr16_le(ev.data() + 2, seq);
  wire::wr32_le(ev.data() + 4, event);   // event window
  wire::wr32_le(ev.data() + 8, window);  // destroyed window
  return ev;
}

// UnmapNotify (type 18)
inline std::array<uint8_t,32> buildUnmapNotify(uint16_t seq,
                                               uint32_t event,
                                               uint32_t window,
                                               bool fromConfigure)
{
  std::array<uint8_t,32> ev{};
  ev.fill(0);
  ev[0] = 18; // UnmapNotify
  wire::wr16_le(ev.data() + 2, seq);
  wire::wr32_le(ev.data() + 4, event);
  wire::wr32_le(ev.data() + 8, window);
  ev[12] = fromConfigure ? 1 : 0;
  return ev;
}

// MapNotify (type 19)
inline std::array<uint8_t,32> buildMapNotify(uint16_t seq,
                                             uint32_t event,
                                             uint32_t window,
                                             bool overrideRedirect)
{
  std::array<uint8_t,32> ev{};
  ev.fill(0);
  ev[0] = 19; // MapNotify
  wire::wr16_le(ev.data() + 2, seq);
  wire::wr32_le(ev.data() + 4, event);
  wire::wr32_le(ev.data() + 8, window);
  ev[12] = overrideRedirect ? 1 : 0;
  return ev;
}

// ConfigureNotify (type 22)
inline std::array<uint8_t,32> buildConfigureNotify(uint16_t seq,
                                                   uint32_t event,
                                                   uint32_t window,
                                                   uint32_t aboveSibling,
                                                   int16_t x, int16_t y,
                                                   uint16_t w, uint16_t h,
                                                   uint16_t borderWidth,
                                                   bool overrideRedirect)
{
  std::array<uint8_t,32> ev{};
  ev.fill(0);
  ev[0] = 22; // ConfigureNotify
  wire::wr16_le(ev.data() + 2, seq);
  wire::wr32_le(ev.data() + 4, event);
  wire::wr32_le(ev.data() + 8, window);
  wire::wr32_le(ev.data() + 12, aboveSibling); // above-sibling (None=0)
  wire::wr16_le(ev.data() + 16, (uint16_t)x);
  wire::wr16_le(ev.data() + 18, (uint16_t)y);
  wire::wr16_le(ev.data() + 20, w);
  wire::wr16_le(ev.data() + 22, h);
  wire::wr16_le(ev.data() + 24, borderWidth);
  ev[26] = overrideRedirect ? 1 : 0;
  return ev;
}

// CirculateNotify (type 26)
inline std::array<uint8_t,32> buildCirculateNotify(uint16_t seq,
                                                   uint32_t event,
                                                   uint32_t window,
                                                   uint8_t place) // 0=Top, 1=Bottom
{
  std::array<uint8_t,32> ev{};
  ev.fill(0);
  ev[0] = 26; // CirculateNotify
  wire::wr16_le(ev.data() + 2, seq);
  wire::wr32_le(ev.data() + 4, event);
  wire::wr32_le(ev.data() + 8, window);
  // bytes 12-15 unused
  ev[16] = place;
  return ev;
}

} // namespace x11::wireev
