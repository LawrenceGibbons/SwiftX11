//
//  X11Modifiers.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#pragma once
#include <cstdint>

// Command-as-Control toggle (SwiftBridge.cpp).  When on, ⌘ reports as X11
// Control in the event state so ⌘C/⌘V drive X11 copy/paste.
extern "C" int x11_get_cmd_as_ctrl(void);

namespace x11::input {

// Internal, cross-platform modifier bits
enum ModifierBits : uint32_t {
  Shift = 1u << 0,
  Ctrl  = 1u << 1,
  Alt   = 1u << 2,
  Cmd   = 1u << 3,
  Lock  = 1u << 4,
};

// Convert internal modifier bits → X11 event state mask
inline uint16_t toX11State(uint32_t buttons, uint32_t mods) {
  uint16_t st = 0;

  // X11 modifier masks
  if (mods & Shift) st |= (1u << 0); // ShiftMask
  if (mods & Lock)  st |= (1u << 1); // LockMask (CapsLock)
  if (mods & Ctrl)  st |= (1u << 2); // ControlMask
  if (mods & Alt)   st |= (1u << 3); // Mod1Mask
  // ⌘ → Control (⌘C/⌘V copy/paste) when the toggle is on, else Super/Mod4.
  if (mods & Cmd)   st |= x11_get_cmd_as_ctrl() ? (1u << 2) : (1u << 6);

  // Buttons: Button1Mask starts at bit 8
  // Your `buttons` bit layout is 1<<(button-1), so map 1..5 => bits 8..12
  for (int i = 0; i < 5; i++) {
    if (buttons & (1u << (uint32_t)i)) {
      st |= (uint16_t)(1u << (8 + i));
    }
  }


  return st;
}

} // namespace x11::input
