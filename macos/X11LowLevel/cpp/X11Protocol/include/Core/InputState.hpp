//
//  InputState.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/4/26.
//

#pragma once
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace x11 {

  struct InputState {
    // global/root pointer
    int32_t root_x_u = 0, root_y_u = 0;

    // host-window local
    int32_t win_x_u = 0,  win_y_u = 0;

    // canonical state
    uint32_t buttons = 0;
    uint32_t mods = 0;

    uint32_t last_xid = 0;    // host window xid last used for motion
    uint32_t pointer_xid = 0; // pointer owner (enter/click)
    uint32_t focus_xid = 0;   // current X input focus window
    uint32_t focus_host = 0;  // top-level host that currently has Cocoa focus (optional)
    uint32_t drag_xid = 0;    // active grab window (nonzero buttons)
    uint32_t last_cursor_host = 0;
    uint32_t last_cursor_cid  = 0;

    // Screen origin cache: maps host XID → (screen_x, screen_y).
    // Updated whenever the pointer moves over a host window.
    // screen_origin = root_coords - window_local_coords.
    // Used by QueryPointer to compute window-local coords for windows
    // on a different host than the pointer's current location.
    std::unordered_map<uint32_t, std::pair<int32_t,int32_t>> hostOrigins;

    // Look up a host window's cached screen origin.
    // Returns false if the host hasn't been visited yet.
    bool getHostOrigin(uint32_t host_xid, int32_t& ox, int32_t& oy) const {
      auto it = hostOrigins.find(host_xid);
      if (it == hostOrigins.end()) return false;
      ox = it->second.first;
      oy = it->second.second;
      return true;
    }

    inline void setFocus(uint32_t host, uint32_t xid) {
      focus_host = host;
      focus_xid  = xid;
    }

    inline uint32_t routeKey(uint32_t host_xid) const {
      if (host_xid != 0 && focus_host == host_xid && focus_xid != 0) return focus_xid;
      return host_xid;
    }


    void updateMotion(uint32_t xid,
                      int32_t wx_u, int32_t wy_u,
                      int32_t rx_u, int32_t ry_u,
                      uint32_t btns, uint32_t m)
    {
      last_xid = xid;
      win_x_u = wx_u; win_y_u = wy_u;
      root_x_u = rx_u; root_y_u = ry_u;
      // Do NOT overwrite `buttons` from motion events.  button() is the
      // canonical authority for button state.  A PointerMove with deliver=0
      // (e.g. tracking-area update after mouseUp) can carry a stale
      // buttons=0 that would silently reset the field between press and
      // release, preventing drag_xid from clearing.
      mods = m;

      // Cache this host's screen origin (root_pos - window_local_pos).
      // QueryPointer uses this to compute coords for windows on other hosts.
      if (xid != 0) {
        hostOrigins[xid] = {rx_u - wx_u, ry_u - wy_u};
      }
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

    // MARK: -- buttons, ponters and cursors
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

#ifndef NDEBUG
      fprintf(stderr,
              "[INPUT_BTN] xid=0x%08X %s btn=%u before=0x%08X after_mask=0x%08X mask=0x%08X buttons=0x%08X drag=0x%08X->",
              (unsigned)xid, is_press ? "press" : "release",
              (unsigned)button_num, (unsigned)before, (unsigned)after_mask,
              (unsigned)mask, (unsigned)buttons, (unsigned)drag_xid);
#endif

      // Robust drag tracking: don't depend on 'before' being preserved
      // between press and release (a PointerMove can corrupt it).
      //   - All buttons released → always clear drag
      //   - First button down while not dragging → start drag
      if (buttons == 0) {
        drag_xid = 0;
      } else if (drag_xid == 0) {
        drag_xid = xid;
      }

#ifndef NDEBUG
      fprintf(stderr, "0x%08X\n", (unsigned)drag_xid);
#endif
    }

    uint32_t routePointer(uint32_t from_xid) const {
      if (drag_xid) return drag_xid;
      if (pointer_xid) return pointer_xid;
      if (focus_xid) return focus_xid;
      return from_xid;
    }
    
  };
  
} // namespace x11
