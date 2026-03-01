//
//  WindowView.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/25/26.
//

#pragma once
#include <cstdint>

namespace x11 
{
  
// A “view” of an X11 window needed for event emission.
// This is intentionally NOT x11_win_t (keeps C globals isolated).
  
struct WindowView {
  // geometry
  uint32_t xid = 0;
  int16_t  x = 0;
  int16_t  y = 0;
  uint16_t w = 0;
  uint16_t h = 0;

  uint32_t parent_xid = 0;

  // X11 SelectInput mask bits
  uint32_t event_mask = 0;

  // Window background pixel (ARGB8888, alpha forced opaque).
  // Only valid when has_background_pixel is true.
  uint32_t background_pixel = 0;
  bool     has_background_pixel = false;

  // Window border (server-drawn around child windows)
  uint16_t border_width = 0;
  uint32_t border_pixel = 0xFF000000u; // ARGB, default black

  // state flags
  bool mapped = false;
  bool presentable = false;
  bool dirty = false;

  // client socket for this window
  int  owner_fd = -1;
};
  
}


