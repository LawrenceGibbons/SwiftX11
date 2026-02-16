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
    // global/root pointer
    int32_t root_x = 0, root_y = 0;

    // host-window local
    int32_t win_x = 0,  win_y = 0;

    // canonical state
    uint32_t buttons = 0;
    uint32_t mods = 0;

    uint32_t last_xid = 0;    // host window xid last used for motion
    uint32_t pointer_xid = 0; // pointer owner (enter/click)
    uint32_t focus_xid = 0;   // current X input focus window
    uint32_t focus_host = 0;  // top-level host that currently has Cocoa focus (optional)
    uint32_t drag_xid = 0;    // active grab window (nonzero buttons)

    void updateMotion(uint32_t xid,
                      int32_t wx, int32_t wy,
                      int32_t rx, int32_t ry,
                      uint32_t btns, uint32_t m)
    {
      last_xid = xid;
      win_x = wx; win_y = wy;
      root_x = rx; root_y = ry;
      // For motion, trust btns if you want. Or keep canonical `buttons` only.
      // I'd keep canonical `buttons`, but accept btns for now:
      buttons = btns;
      mods = m;
    }

    void setFocusXid(uint32_t xid) { focus_xid = xid; }
    
    void setFocusHost(uint32_t host_xid) {
      focus_host = host_xid;
      // Do NOT set focus_xid here; caller decides (needs WindowTable).
    }

    void clearFocusHost(uint32_t host_xid) {
      if (focus_host == host_xid) focus_host = 0;
      focus_xid = 0;
      // pointer_xid: keep unless you want to clear when not dragging.
      if (drag_xid == 0) pointer_xid = 0;
    }
    
    void enter(uint32_t xid) {
      if (drag_xid == 0) pointer_xid = xid;
    }

    void leave(uint32_t xid) {
      if (drag_xid == 0 && pointer_xid == xid) pointer_xid = 0;
    }

    // This matches your old canonicalization behavior.
    void button(uint32_t xid, bool is_press, uint8_t button_num, uint32_t after_mask) {
      const uint32_t before = buttons;

      // Force bit to match press/release.
      uint32_t mask = after_mask;
      if (button_num >= 1 && button_num <= 31) {
        const uint32_t bit = (1u << (uint32_t)(button_num - 1u));
        if (is_press) mask |= bit;
        else          mask &= ~bit;
      }

      // click implies pointer ownership
      if (is_press) pointer_xid = xid;

      buttons = mask;

      if (before == 0 && buttons != 0) drag_xid = xid;
      else if (before != 0 && buttons == 0) drag_xid = 0;
    }

    uint32_t routePointer(uint32_t from_xid) const {
      if (drag_xid) return drag_xid;
      if (pointer_xid) return pointer_xid;
      if (focus_xid) return focus_xid;
      return from_xid;
    }
  };
  
} // namespace x11
