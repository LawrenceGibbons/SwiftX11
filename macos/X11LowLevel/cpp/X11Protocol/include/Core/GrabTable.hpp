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
  static GrabTable& instance();

  // Passive grabs (GrabButton/UngrabButton)
  void addOrReplace(const PassiveGrab& g);
  void remove(uint32_t grabWindow, uint8_t button, uint16_t modifiers);
  bool match(uint32_t grabWindow, uint8_t button, uint16_t modifiers, PassiveGrab& out) const;

  // Active pointer grab (GrabPointer/UngrabPointer)
  void setPointerGrab(uint32_t grabWindow, bool ownerEvents, uint16_t eventMask);
  void clearPointerGrab();
  bool getPointerGrab(PointerGrab& out) const;

private:
  GrabTable() = default;

  mutable std::mutex mu_;
  std::vector<PassiveGrab> passive_;
  PointerGrab pointer_;
};

} // namespace x11
