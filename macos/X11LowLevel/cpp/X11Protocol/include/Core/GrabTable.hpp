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
  int      owner_fd = -1;    // grabbing client (v1.19.36.17, review §2.5)
  uint32_t grab_time = 0;    // time from the GrabPointer request (0 = CurrentTime)
};

// GrabPointer/GrabKeyboard reply status codes
static constexpr uint8_t kGrabSuccess    = 0;
static constexpr uint8_t kAlreadyGrabbed = 1;

class GrabTable {
public:
  GrabTable() = default;

  // Passive grabs (GrabButton/UngrabButton)
  void addOrReplace(const PassiveGrab& g);
  void remove(uint32_t grabWindow, uint8_t button, uint16_t modifiers);
  bool match(uint32_t grabWindow, uint8_t button, uint16_t modifiers, PassiveGrab& out) const;

  // Active pointer grab (GrabPointer/UngrabPointer).
  // tryPointerGrab returns kGrabSuccess or kAlreadyGrabbed — a grab held
  // by a DIFFERENT client is no longer silently stomped (review §2.5:
  // Java's liberal XUngrabPointer(CurrentTime) was destroying other
  // clients' menu/drag grabs).  Ungrab only releases the caller's own.
  uint8_t tryPointerGrab(uint32_t grabWindow, bool ownerEvents,
                         uint16_t eventMask, int owner_fd, uint32_t time);
  void clearPointerGrab(int owner_fd);   // owner_fd < 0 forces
  bool getPointerGrab(PointerGrab& out) const;
  void updatePointerGrabEventMask(uint16_t eventMask);

  // Active keyboard grab (GrabKeyboard/UngrabKeyboard), same ownership rules.
  uint8_t tryKeyboardGrab(uint32_t grabWindow, int owner_fd);
  uint32_t clearKeyboardGrab(int owner_fd); // returns released window (0 if none/refused)
  uint32_t getKeyboardGrab() const;         // 0 = no active keyboard grab

  // Reset all grabs (call on session teardown — single client mode)
  void clearAll();

  // Remove grabs for a set of destroyed windows (multi-client teardown)
  void removeForWindows(const std::vector<uint32_t>& xids);

  // Release all grabs owned by a disconnecting client (review §6.5 —
  // a dead XDND helper used to leave the active pointer grab installed,
  // freezing all pointer input).
  void clearOwnedBy(int owner_fd);

private:

  mutable std::mutex mu_;
  std::vector<PassiveGrab> passive_;
  PointerGrab pointer_;
  uint32_t keyboard_grab_window_ = 0;
  int      keyboard_grab_fd_ = -1;
};

} // namespace x11
