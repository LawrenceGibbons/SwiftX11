//
//  CoreKeymap.cpp
//  X11LowLevel
//
//  Core-protocol keyboard description shared by GetKeyboardMapping,
//  Get/SetModifierMapping and the XKEYBOARD keymap builder.
//  (Keysym table moved here from QueryOps.cpp, modifier map storage from
//  PointerOps.cpp, v1.20.0.20.)
//

#include "Core/CoreKeymap.hpp"
#include "Core/KeySyms.hpp"

#include <array>
#include <cstring>

// Command-as-Control is implemented purely in the event state field
// (X11Modifiers.hpp toX11State: ⌘ → ControlMask), which is all copy/paste
// accelerators check.  The keymap/modifier-map deliberately stay ⌘ = Super
// here: remapping them too desynced with the XKB virtual-modifier map (which
// still lists ⌘ under Super), corrupting Java AWT's modifier tracking and
// breaking its menus (v2.0.0.16).

namespace x11 {

// ---------------------------------------------------------------------------
// Keycode → keysym table
// ---------------------------------------------------------------------------

// File-scope so setCoreKeyboardMap (ChangeKeyboardMapping, C9) can mutate the
// very table GetKeyboardMapping serves.
static KeySyms4 g_keyMap[256];
static bool g_keyMapInited = false;

const KeySyms4* coreKeyboardMap() {
  KeySyms4* map = g_keyMap;
  if (g_keyMapInited) return map;
  g_keyMapInited = true;

  // Default: NoSymbol for all columns
  for (auto &e : g_keyMap) { e.syms[0] = e.syms[1] = e.syms[2] = e.syms[3] = XK_NoSymbol; }

  // Set a key with normal and shifted keysyms (columns 3-4 = NoSymbol)
  auto setMac = [&](uint8_t mac_vk, uint32_t lo, uint32_t hi) {
    const uint8_t kc = macToX11Keycode(mac_vk);
    map[kc].syms[0] = lo;
    map[kc].syms[1] = hi;
  };

  // Set a key with same keysym for normal and shifted
  auto setMac1 = [&](uint8_t mac_vk, uint32_t sym) {
    const uint8_t kc = macToX11Keycode(mac_vk);
    map[kc].syms[0] = sym;
    map[kc].syms[1] = sym;
  };

  // ---------- Letters (US layout) ----------
  setMac(0,  'a', 'A');   // kVK_ANSI_A
  setMac(1,  's', 'S');   // kVK_ANSI_S
  setMac(2,  'd', 'D');   // kVK_ANSI_D
  setMac(3,  'f', 'F');   // kVK_ANSI_F
  setMac(4,  'h', 'H');   // kVK_ANSI_H
  setMac(5,  'g', 'G');   // kVK_ANSI_G
  setMac(6,  'z', 'Z');   // kVK_ANSI_Z
  setMac(7,  'x', 'X');   // kVK_ANSI_X
  setMac(8,  'c', 'C');   // kVK_ANSI_C
  setMac(9,  'v', 'V');   // kVK_ANSI_V
  setMac(11, 'b', 'B');   // kVK_ANSI_B
  setMac(12, 'q', 'Q');   // kVK_ANSI_Q
  setMac(13, 'w', 'W');   // kVK_ANSI_W
  setMac(14, 'e', 'E');   // kVK_ANSI_E
  setMac(15, 'r', 'R');   // kVK_ANSI_R
  setMac(16, 'y', 'Y');   // kVK_ANSI_Y
  setMac(17, 't', 'T');   // kVK_ANSI_T
  setMac(31, 'o', 'O');   // kVK_ANSI_O
  setMac(32, 'u', 'U');   // kVK_ANSI_U
  setMac(34, 'i', 'I');   // kVK_ANSI_I
  setMac(35, 'p', 'P');   // kVK_ANSI_P
  setMac(37, 'l', 'L');   // kVK_ANSI_L
  setMac(38, 'j', 'J');   // kVK_ANSI_J
  setMac(40, 'k', 'K');   // kVK_ANSI_K
  setMac(45, 'n', 'N');   // kVK_ANSI_N
  setMac(46, 'm', 'M');   // kVK_ANSI_M

  // ---------- Digits & symbols ----------
  setMac(18, '1', '!');   // kVK_ANSI_1
  setMac(19, '2', '@');   // kVK_ANSI_2
  setMac(20, '3', '#');   // kVK_ANSI_3
  setMac(21, '4', '$');   // kVK_ANSI_4
  setMac(22, '6', '^');   // kVK_ANSI_6
  setMac(23, '5', '%');   // kVK_ANSI_5
  setMac(24, '=', '+');   // kVK_ANSI_Equal
  setMac(25, '9', '(');   // kVK_ANSI_9
  setMac(26, '7', '&');   // kVK_ANSI_7
  setMac(27, '-', '_');   // kVK_ANSI_Minus
  setMac(28, '8', '*');   // kVK_ANSI_8
  setMac(29, '0', ')');   // kVK_ANSI_0

  // ---------- Punctuation ----------
  setMac(30, ']', '}');   // kVK_ANSI_RightBracket
  setMac(33, '[', '{');   // kVK_ANSI_LeftBracket
  setMac(39, '\'', '"');  // kVK_ANSI_Quote
  setMac(41, ';', ':');   // kVK_ANSI_Semicolon
  setMac(42, '\\', '|'); // kVK_ANSI_Backslash
  setMac(43, ',', '<');   // kVK_ANSI_Comma
  setMac(44, '/', '?');   // kVK_ANSI_Slash
  setMac(47, '.', '>');   // kVK_ANSI_Period
  setMac(50, '`', '~');   // kVK_ANSI_Grave
  setMac(10, XK_section, XK_plusminus); // kVK_ISO_Section (§/±)

  // ---------- Whitespace / control ----------
  setMac1(36, XK_Return);    // kVK_Return
  setMac1(48, XK_Tab);       // kVK_Tab
  setMac1(49, XK_space);     // kVK_Space
  setMac1(51, XK_BackSpace); // kVK_Delete (backspace)
  setMac1(53, XK_Escape);    // kVK_Escape

  // ---------- Modifiers ----------
  setMac1(56, XK_Shift_L);    // kVK_Shift
  setMac1(60, XK_Shift_R);    // kVK_RightShift
  setMac1(59, XK_Control_L);  // kVK_Control
  setMac1(62, XK_Control_R);  // kVK_RightControl
  setMac1(58, XK_Alt_L);      // kVK_Option
  setMac1(61, XK_Alt_R);      // kVK_RightOption
  setMac1(55, XK_Super_L);    // kVK_Command   (⌘ acts as Control via the event state, not the keymap)
  setMac1(54, XK_Super_R);    // kVK_RightCommand
  setMac1(57, XK_Caps_Lock);  // kVK_CapsLock
  setMac1(63, XK_Meta_L);     // kVK_Function (Fn key → Meta_L)

  // ---------- Arrow keys ----------
  setMac1(123, XK_Left);   // kVK_LeftArrow
  setMac1(124, XK_Right);  // kVK_RightArrow
  setMac1(125, XK_Down);   // kVK_DownArrow
  setMac1(126, XK_Up);     // kVK_UpArrow

  // ---------- Navigation ----------
  setMac1(115, XK_Home);      // kVK_Home
  setMac1(117, XK_End);       // kVK_End
  setMac1(116, XK_Page_Up);   // kVK_PageUp
  setMac1(121, XK_Page_Down); // kVK_PageDown
  setMac1(119, XK_Delete);    // kVK_ForwardDelete
  setMac1(114, XK_Help);      // kVK_Help (Insert on PC keyboards)

  // ---------- Function keys ----------
  setMac1(122, XK_F1);   // kVK_F1
  setMac1(120, XK_F2);   // kVK_F2
  setMac1(99,  XK_F3);   // kVK_F3
  setMac1(118, XK_F4);   // kVK_F4
  setMac1(96,  XK_F5);   // kVK_F5
  setMac1(97,  XK_F6);   // kVK_F6
  setMac1(98,  XK_F7);   // kVK_F7
  setMac1(100, XK_F8);   // kVK_F8
  setMac1(101, XK_F9);   // kVK_F9
  setMac1(109, XK_F10);  // kVK_F10
  setMac1(103, XK_F11);  // kVK_F11
  setMac1(111, XK_F12);  // kVK_F12
  setMac1(105, XK_F13);  // kVK_F13
  setMac1(107, XK_F14);  // kVK_F14
  setMac1(113, XK_F15);  // kVK_F15
  setMac1(106, XK_F16);  // kVK_F16
  setMac1(64,  XK_F17);  // kVK_F17
  setMac1(79,  XK_F18);  // kVK_F18
  setMac1(80,  XK_F19);  // kVK_F19
  setMac1(90,  XK_F20);  // kVK_F20

  // ---------- Keypad ----------
  // NOTE: column 2 must stay NoSymbol on the KP_7 keycode — Java AWT's
  // IsXsunKPBehavior sniffer treats KP_7 at index 2 as the Sun keypad
  // convention and switches its keypad indexing for the process lifetime.
  setMac1(82,  XK_KP_0);        // kVK_ANSI_Keypad0
  setMac1(83,  XK_KP_1);        // kVK_ANSI_Keypad1
  setMac1(84,  XK_KP_2);        // kVK_ANSI_Keypad2
  setMac1(85,  XK_KP_3);        // kVK_ANSI_Keypad3
  setMac1(86,  XK_KP_4);        // kVK_ANSI_Keypad4
  setMac1(87,  XK_KP_5);        // kVK_ANSI_Keypad5
  setMac1(88,  XK_KP_6);        // kVK_ANSI_Keypad6
  setMac1(89,  XK_KP_7);        // kVK_ANSI_Keypad7
  setMac1(91,  XK_KP_8);        // kVK_ANSI_Keypad8
  setMac1(92,  XK_KP_9);        // kVK_ANSI_Keypad9
  setMac1(65,  XK_KP_Decimal);  // kVK_ANSI_KeypadDecimal
  setMac1(67,  XK_KP_Multiply); // kVK_ANSI_KeypadMultiply
  setMac1(69,  XK_KP_Add);      // kVK_ANSI_KeypadPlus
  setMac1(71,  XK_Clear);       // kVK_ANSI_KeypadClear (NumLock on PC)
  setMac1(75,  XK_KP_Divide);   // kVK_ANSI_KeypadDivide
  setMac1(76,  XK_KP_Enter);    // kVK_ANSI_KeypadEnter
  setMac1(78,  XK_KP_Subtract); // kVK_ANSI_KeypadMinus
  setMac1(81,  XK_KP_Equal);    // kVK_ANSI_KeypadEquals

  return map;
}

// ChangeKeyboardMapping (opcode 100) — C9.  Overwrite `keycodeCount` rows
// starting at `firstKeycode` with the client's keysyms.  Our table has a fixed
// 4 columns (normal/shift/mode/mode+shift); a client sending more is truncated
// to 4, fewer is padded with NoSymbol.  Returns false (→ BadValue) for a range
// outside [min,max].  NOTE: the XKB model (Core/XkbKeymap.cpp) is built once
// and is NOT rebuilt here, so XKB-path clients keep the old keysyms until the
// dynamic rebuild (E2 / R5); core-path clients (GetKeyboardMapping) see this
// immediately.
bool setCoreKeyboardMap(uint8_t firstKeycode, uint8_t keysymsPerKeycode,
                        uint8_t keycodeCount, const uint32_t* syms) {
  (void)coreKeyboardMap();   // ensure the table is seeded
  if (!syms || keycodeCount == 0 || keysymsPerKeycode == 0) return false;
  if (firstKeycode < kCoreMinKeyCode) return false;
  if ((int)firstKeycode + (int)keycodeCount - 1 > (int)kCoreMaxKeyCode) return false;

  for (uint8_t k = 0; k < keycodeCount; k++) {
    KeySyms4& e = g_keyMap[(uint8_t)(firstKeycode + k)];
    for (uint8_t c = 0; c < kCoreKeysymsPerKeycode; c++) {
      e.syms[c] = (c < keysymsPerKeycode)
                    ? syms[(size_t)k * keysymsPerKeycode + c]
                    : (uint32_t)XK_NoSymbol;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Modifier map (core 118/119)
// ---------------------------------------------------------------------------
// Protocol: 8 modifiers, each has numKeyPerModifier keycodes (CARD8).
// Storage: g_modMap[modifier_index * n + key_index].

static uint8_t g_modMapN = 0;
static std::array<uint8_t, 8 * kCoreMaxKeysPerModifier> g_modMap{};

// Seed a reasonable default modifier map (n=2 keys per modifier: left + right).
static void initDefaultModifierMapIfEmpty() {
  // If we've already been set to something nonzero, don't clobber it.
  if (g_modMapN != 0) return;

  g_modMapN = 2;
  g_modMap.fill(0);

  // Order: Shift, Lock, Control, Mod1, Mod2, Mod3, Mod4, Mod5
  g_modMap[0*2 + 0] = macToX11Keycode(56);  // Shift_L
  g_modMap[0*2 + 1] = macToX11Keycode(60);  // Shift_R
  g_modMap[1*2 + 0] = macToX11Keycode(57);  // CapsLock
  g_modMap[1*2 + 1] = 0;                    // (no second Lock key)
  g_modMap[2*2 + 0] = macToX11Keycode(59);  // Control_L
  g_modMap[2*2 + 1] = macToX11Keycode(62);  // Control_R
  g_modMap[3*2 + 0] = macToX11Keycode(58);  // Option_L  -> Mod1 (Alt)
  g_modMap[3*2 + 1] = macToX11Keycode(61);  // Option_R  -> Mod1 (Alt)
  g_modMap[4*2 + 0] = 0;                    // Mod2 (unused)
  g_modMap[4*2 + 1] = 0;
  g_modMap[5*2 + 0] = 0;                    // Mod3 (unused)
  g_modMap[5*2 + 1] = 0;
  g_modMap[6*2 + 0] = macToX11Keycode(55);  // Command_L -> Mod4 (Super)
  g_modMap[6*2 + 1] = macToX11Keycode(54);  // Command_R -> Mod4 (Super)
  g_modMap[7*2 + 0] = 0;                    // Mod5 (unused)
  g_modMap[7*2 + 1] = 0;
}

const uint8_t* coreModifierMap(uint8_t& keysPerMod) {
  initDefaultModifierMapIfEmpty();
  keysPerMod = g_modMapN;
  return g_modMap.data();
}

bool setCoreModifierMap(const uint8_t* keys, uint8_t keysPerMod) {
  if (!keys || keysPerMod == 0 || keysPerMod > kCoreMaxKeysPerModifier) return false;
  g_modMapN = keysPerMod;
  g_modMap.fill(0);
  std::memcpy(g_modMap.data(), keys, (size_t)keysPerMod * 8u);
  return true;
}

} // namespace x11
