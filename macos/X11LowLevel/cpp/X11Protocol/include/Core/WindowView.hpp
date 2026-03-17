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
  uint32_t xi2_mask = 0;    // XI2 event selection mask

  // Window background pixel (ARGB8888, alpha forced opaque).
  // Only valid when has_background_pixel is true.
  uint32_t background_pixel = 0;
  bool     has_background_pixel = false;
  bool     is_parent_relative = false;  // CWBackPixmap=ParentRelative
  uint32_t background_pixmap = 0;       // CWBackPixmap pixmap XID (0=none)

  // Window border (server-drawn around child windows)
  uint16_t border_width = 0;
  uint32_t border_pixel = 0xFF000000u; // ARGB, default black

  // Window manager attributes
  bool     override_redirect = false;  // CWOverrideRedirect (bit 9)
  uint8_t  win_gravity = 1;   // CWWinGravity (bit 5): default NorthWest=1
  uint8_t  bit_gravity = 0;   // CWBitGravity (bit 4): default Forget=0
  uint8_t  backing_store = 0; // CWBackingStore (bit 6): default NotUseful=0

  // state flags
  bool mapped = false;
  bool presentable = false;
  bool dirty = false;
  bool surface_resize_exposed = false; // true after initial SurfaceResized re-expose

  // client socket for this window
  int  owner_fd = -1;

  // ICCCM WM_HINTS / WM_PROTOCOLS cached flags
  bool wants_input = true;       // WM_HINTS input hint (default: passively focusable)
  bool wants_take_focus = false;  // WM_PROTOCOLS contains WM_TAKE_FOCUS

  // SHAPE extension: non-rectangular window flags
  bool bounding_shaped = false;
  bool clip_shaped = false;
  bool input_shaped = false;
};
  
}


