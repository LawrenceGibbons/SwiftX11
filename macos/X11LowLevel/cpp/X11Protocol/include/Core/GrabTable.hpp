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
  uint16_t eventMask = 0;        // core mask installed when the grab activates
};

// Active pointer grab.  Carries the fields of xorg's GrabRec that delivery
// consults (dix/events.c GrabDevice / DeliverGrabbedEvent /
// ActivateImplicitGrab): the grab's own mask at its own level, who holds it,
// and whether it was activated by a button press.
struct PointerGrab {
  bool     active = false;
  uint32_t grabWindow = 0;
  bool     ownerEvents = false;
  uint16_t eventMask = 0;      // core mask: grabtype CORE, or an implicit grab's delivery mask
  int      owner_fd = -1;      // grabbing client (v1.19.36.17, review §2.5)
  uint32_t grab_time = 0;      // server time at activation (CurrentTime resolved to now)
  // Phase B (v1.20.0.17, docs/XI2_XORG_COMPARISON.md M3/M7/M17)
  bool     is_xi2 = false;     // grabtype XI2 (XIGrabDevice / implicit from an XI2 press)
  uint32_t xi2mask = 0;        // XI2 mask word 0 for XI2-level delivery
  bool     implicit = false;   // activated by a press (implicit or passive) — released on last release
  uint8_t  pointer_mode = 1;   // GrabModeAsync (sync grabs are not frozen — see G-11)
  uint8_t  keyboard_mode = 1;
  uint32_t cursor = 0;         // grab cursor (stored; not yet applied — L15)
};

struct KeyboardGrab {
  bool     active = false;
  uint32_t grabWindow = 0;
  bool     ownerEvents = false;
  int      owner_fd = -1;
  uint32_t grab_time = 0;
  bool     is_xi2 = false;
  uint32_t xi2mask = 0;
};

// Grab status codes (core GrabPointer/GrabKeyboard reply; XI2 uses the same
// values — XIGrabSuccess/XIAlreadyGrabbed/XIGrabInvalidTime/XIGrabNotViewable/
// XIGrabFrozen in XI2.h).
static constexpr uint8_t kGrabSuccess     = 0;
static constexpr uint8_t kAlreadyGrabbed  = 1;
static constexpr uint8_t kGrabInvalidTime = 2;
static constexpr uint8_t kGrabNotViewable = 3;
static constexpr uint8_t kGrabFrozen      = 4;

// X11 timestamps are 32-bit milliseconds and wrap; CurrentTime is 0.
inline bool timeIsLater(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

// xorg GrabDevice (dix/events.c:5261-5263): GrabInvalidTime when the request
// time is later than now, or earlier than the time of the grab currently held.
inline bool grabTimeValid(uint32_t t, uint32_t now, uint32_t heldGrabTime) {
  if (t == 0) return true;                                 // CurrentTime
  if (timeIsLater(t, now)) return false;
  if (heldGrabTime != 0 && timeIsLater(heldGrabTime, t)) return false;
  return true;
}

// xorg ProcUngrabPointer / ProcXIUngrabDevice / ProcChangeActivePointerGrab
// (dix/events.c:5136-5138, 5166-5168; Xi/xigrabdev.c:165-168): act only when
// the request time is not later than now and not earlier than the grab time.
inline bool ungrabTimeValid(uint32_t t, uint32_t now, uint32_t heldGrabTime) {
  const uint32_t tt = (t == 0) ? now : t;
  if (timeIsLater(tt, now)) return false;
  if (heldGrabTime != 0 && timeIsLater(heldGrabTime, tt)) return false;
  return true;
}

class GrabTable {
public:
  GrabTable() = default;

  // Passive grabs (GrabButton/UngrabButton)
  void addOrReplace(const PassiveGrab& g);
  void remove(uint32_t grabWindow, uint8_t button, uint16_t modifiers);
  bool match(uint32_t grabWindow, uint8_t button, uint16_t modifiers, PassiveGrab& out) const;

  // Active pointer grab.  Returns kGrabSuccess or kAlreadyGrabbed: a grab
  // held by ANOTHER client, or by this client at the OTHER level (xorg
  // GrabDevice: `grab->grabtype != grabtype` → AlreadyGrabbed), is refused
  // (review §2.5: Java's liberal XUngrabPointer(CurrentTime) was destroying
  // other clients' menu/drag grabs).  Viewability and time checks belong to
  // the caller (they need the window table and the clock).
  uint8_t tryPointerGrab(const PointerGrab& req);
  // Core convenience (grabtype CORE, explicit).
  uint8_t tryPointerGrab(uint32_t grabWindow, bool ownerEvents,
                         uint16_t eventMask, int owner_fd, uint32_t time);
  void clearPointerGrab(int owner_fd);   // owner_fd < 0 forces
  bool getPointerGrab(PointerGrab& out) const;
  void updatePointerGrabEventMask(uint16_t eventMask);

  // Active keyboard grab (GrabKeyboard/XIGrabDevice on a keyboard), same
  // ownership and level rules.
  uint8_t tryKeyboardGrab(const KeyboardGrab& req);
  uint8_t tryKeyboardGrab(uint32_t grabWindow, int owner_fd);   // core convenience
  uint32_t clearKeyboardGrab(int owner_fd); // returns released window (0 if none/refused)
  uint32_t getKeyboardGrab() const;         // 0 = no active keyboard grab
  bool getKeyboardGrabInfo(KeyboardGrab& out) const;

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
  KeyboardGrab keyboard_;
};

} // namespace x11
