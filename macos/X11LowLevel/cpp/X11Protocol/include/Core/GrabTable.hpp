//
//  GrabTable.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/17/26.
//

#pragma once
#include <cstdint>
#include <vector>
#include <mutex>

namespace x11 {

// X11 constants
static constexpr uint8_t  AnyButton    = 0;       // 0 means AnyButton
static constexpr uint16_t AnyModifier  = 0x8000u; // AnyModifier per X11 protocol

struct PassiveGrab {
  uint32_t grabWindow = 0;
  uint8_t  button = AnyButton;   // 0 => AnyButton
  uint16_t modifiers = AnyModifier; // 0x8000 => AnyModifier
  bool ownerEvents = false;
  uint16_t eventMask = 0;        // optional, keep for later
};

struct PointerGrab {
  bool active = false;
  uint32_t grabWindow = 0;
  bool ownerEvents = false;
  uint16_t eventMask = 0;
};

class GrabTable {
public:
  GrabTable() = default;

  // Passive grabs (GrabButton/UngrabButton)
  void addOrReplace(const PassiveGrab& g);
  void remove(uint32_t grabWindow, uint8_t button, uint16_t modifiers);
  bool match(uint32_t grabWindow, uint8_t button, uint16_t modifiers, PassiveGrab& out) const;

  // Active pointer grab (GrabPointer/UngrabPointer)
  void setPointerGrab(uint32_t grabWindow, bool ownerEvents, uint16_t eventMask);
  void clearPointerGrab();
  bool getPointerGrab(PointerGrab& out) const;
  void updatePointerGrabEventMask(uint16_t eventMask);

  // Active keyboard grab (GrabKeyboard/UngrabKeyboard)
  void setKeyboardGrab(uint32_t grabWindow);
  uint32_t clearKeyboardGrab();  // returns previous grab window (0 if none)

  // Reset all grabs (call on session teardown — single client mode)
  void clearAll();

  // Remove grabs for a set of destroyed windows (multi-client teardown)
  void removeForWindows(const std::vector<uint32_t>& xids);

private:

  mutable std::mutex mu_;
  std::vector<PassiveGrab> passive_;
  PointerGrab pointer_;
  uint32_t keyboard_grab_window_ = 0;
};

} // namespace x11
