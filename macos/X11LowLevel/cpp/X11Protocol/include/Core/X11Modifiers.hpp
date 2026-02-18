//
//  X11Modifiers.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#pragma once
#include <cstdint>

namespace x11::input {

// Internal, cross-platform modifier bits
enum ModifierBits : uint32_t {
  Shift = 1u << 0,
  Ctrl  = 1u << 1,
  Alt   = 1u << 2,
  Cmd   = 1u << 3,
};

// Convert internal modifier bits → X11 event state mask
inline uint16_t toX11State(uint32_t buttons, uint32_t mods) {
  uint16_t st = 0;

  // X11 modifier masks
  if (mods & Shift) st |= (1u << 0); // ShiftMask
  if (mods & Ctrl)  st |= (1u << 2); // ControlMask
  if (mods & Alt)   st |= (1u << 3); // Mod1Mask
  if (mods & Cmd)   st |= (1u << 6); // Mod4Mask (Super/Command)

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
