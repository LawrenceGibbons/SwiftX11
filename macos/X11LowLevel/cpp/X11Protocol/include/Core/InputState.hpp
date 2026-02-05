//
//  InputState.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/4/26.
//

#pragma once
#include <cstdint>

namespace x11 {

struct InputState {
  int32_t root_x = 0, root_y = 0;
  int32_t win_x = 0,  win_y = 0;
  uint32_t buttons = 0;
  uint32_t mods = 0;

  // Which host window xid last provided motion (good-enough targeting for now)
  uint32_t last_xid = 0;

  void update(uint32_t xid,
              int32_t wx, int32_t wy,
              int32_t rx, int32_t ry,
              uint32_t btns, uint32_t m)
  {
    last_xid = xid;
    win_x = wx; win_y = wy;
    root_x = rx; root_y = ry;
    buttons = btns;
    mods = m;
  }
};

} // namespace x11
