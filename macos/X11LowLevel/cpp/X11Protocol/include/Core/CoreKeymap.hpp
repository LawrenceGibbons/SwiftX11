//
//  CoreKeymap.hpp
//  X11LowLevel
//
//  The server's single keyboard description at the core-protocol level:
//  the keycode → keysym table served by GetKeyboardMapping (opcode 101) and
//  the modifier map served by Get/SetModifierMapping (118/119).
//
//  XKEYBOARD (Extensions/XKBOps.cpp, Core/XkbKeymap.cpp) derives its key
//  types, symbol maps and modmap from these same tables, so the core and
//  XKB views of the keyboard can never disagree — libX11, GDK, Java AWT and
//  Chromium all translate keycodes through XKB once the extension is
//  advertised but still consult the core tables for modifier detection
//  (AWT's setupModifierMap, Xlib's XKeysymToKeycode range walk), and a
//  mismatch between the two shows up as dead or mis-shifted keys.
//

#pragma once
#include <cstdint>

namespace x11 {

// 4 keysyms per keycode: normal, shift, mode_switch, mode_switch+shift.
// Java Swing / GTK expect 4 columns; Mode_switch columns are NoSymbol on macOS.
struct KeySyms4 { uint32_t syms[4]; };

static constexpr uint8_t kCoreKeysymsPerKeycode = 4;

// Connection-setup keycode range (X11Setup.cpp advertises 8..255).  XKB's
// GetMap must report exactly the same range: GDK indexes the XKB arrays with
// the setup range, and libX11's XKeysymToKeycode walks the setup range over
// arrays sized by the XKB range.
static constexpr uint8_t kCoreMinKeyCode = 8;
static constexpr uint8_t kCoreMaxKeyCode = 255;

// Modifier map storage cap (keys per modifier row).
static constexpr uint8_t kCoreMaxKeysPerModifier = 32;

// X11 keycodes are macOS virtual keycodes + 8.
inline uint8_t macToX11Keycode(uint8_t mac_vk) { return (uint8_t)(mac_vk + 8u); }

// 256-entry table indexed by X11 keycode (US layout on macOS keycodes).
const KeySyms4* coreKeyboardMap();

// Packed modifier map: 8 rows (Shift, Lock, Control, Mod1..Mod5) of
// `keysPerMod` keycodes each; 0 = empty slot.  Seeds the macOS default
// (Shift/CapsLock/Control/Option→Mod1/Command→Mod4) on first use.
const uint8_t* coreModifierMap(uint8_t& keysPerMod);

// SetModifierMapping: replace the whole map.  Returns false (MappingFailed)
// when keysPerMod is 0 or exceeds the storage cap.
bool setCoreModifierMap(const uint8_t* keys, uint8_t keysPerMod);

} // namespace x11
