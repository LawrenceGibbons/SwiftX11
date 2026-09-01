//
//  GrabTable.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/17/26.
//

#include "Core/GrabTable.hpp"
#include <algorithm>

namespace x11 {

void GrabTable::addOrReplace(const PassiveGrab& g) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& e : passive_) {
    if (e.grabWindow == g.grabWindow && e.button == g.button && e.modifiers == g.modifiers) {
      e = g;
      return;
    }
  }
  passive_.push_back(g);
}

void GrabTable::remove(uint32_t grabWindow, uint8_t button, uint16_t modifiers) {
  std::lock_guard<std::mutex> lock(mu_);
  for (size_t i = 0; i < passive_.size(); ) {
    const auto& e = passive_[i];
    if (e.grabWindow == grabWindow &&
        (button == AnyButton || e.button == button) &&
        (modifiers == AnyModifier || e.modifiers == modifiers)) {
      passive_.erase(passive_.begin() + (long)i);
      continue;
    }
    i++;
  }
}

bool GrabTable::match(uint32_t grabWindow, uint8_t button, uint16_t modifiers, PassiveGrab& out) const {
  std::lock_guard<std::mutex> lock(mu_);

  // Prefer most-specific match: exact button+mods > anybutton/exactmods > exactbutton/anymods > any/any
  auto score = [&](const PassiveGrab& g) -> int {
    if (g.grabWindow != grabWindow) return -1;
    const bool btnOK = (g.button == AnyButton || g.button == button);
    const bool modOK = (g.modifiers == AnyModifier || g.modifiers == modifiers);
    if (!btnOK || !modOK) return -1;

    int s = 0;
    if (g.button != AnyButton) s += 2;
    if (g.modifiers != AnyModifier) s += 1;
    return s;
  };

  int bestS = -1;
  const PassiveGrab* best = nullptr;
  for (const auto& g : passive_) {
    const int s = score(g);
    if (s > bestS) { bestS = s; best = &g; }
  }
  if (!best) return false;
  out = *best;
  return true;
}

uint8_t GrabTable::tryPointerGrab(uint32_t grabWindow, bool ownerEvents,
                                  uint16_t eventMask, int owner_fd,
                                  uint32_t time) {
  std::lock_guard<std::mutex> lock(mu_);
  if (pointer_.active && pointer_.owner_fd >= 0 &&
      pointer_.owner_fd != owner_fd) {
    return kAlreadyGrabbed;
  }
  pointer_.active = true;
  pointer_.grabWindow = grabWindow;
  pointer_.ownerEvents = ownerEvents;
  pointer_.eventMask = eventMask;
  pointer_.owner_fd = owner_fd;
  pointer_.grab_time = time;
  return kGrabSuccess;
}

void GrabTable::clearPointerGrab(int owner_fd) {
  std::lock_guard<std::mutex> lock(mu_);
  if (owner_fd >= 0 && pointer_.active &&
      pointer_.owner_fd >= 0 && pointer_.owner_fd != owner_fd) {
    return; // another client's grab — not yours to release
  }
  pointer_ = PointerGrab{};
}

bool GrabTable::getPointerGrab(PointerGrab& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  out = pointer_;
  return out.active;
}

void GrabTable::updatePointerGrabEventMask(uint16_t eventMask) {
  std::lock_guard<std::mutex> lock(mu_);
  if (pointer_.active) {
    pointer_.eventMask = eventMask;
  }
}

uint8_t GrabTable::tryKeyboardGrab(uint32_t grabWindow, int owner_fd) {
  std::lock_guard<std::mutex> lock(mu_);
  if (keyboard_grab_window_ != 0 && keyboard_grab_fd_ >= 0 &&
      keyboard_grab_fd_ != owner_fd) {
    return kAlreadyGrabbed;
  }
  keyboard_grab_window_ = grabWindow;
  keyboard_grab_fd_ = owner_fd;
  return kGrabSuccess;
}

uint32_t GrabTable::clearKeyboardGrab(int owner_fd) {
  std::lock_guard<std::mutex> lock(mu_);
  if (owner_fd >= 0 && keyboard_grab_window_ != 0 &&
      keyboard_grab_fd_ >= 0 && keyboard_grab_fd_ != owner_fd) {
    return 0; // another client's grab
  }
  uint32_t prev = keyboard_grab_window_;
  keyboard_grab_window_ = 0;
  keyboard_grab_fd_ = -1;
  return prev;
}

uint32_t GrabTable::getKeyboardGrab() const {
  std::lock_guard<std::mutex> lock(mu_);
  return keyboard_grab_window_;
}

void GrabTable::clearAll() {
  std::lock_guard<std::mutex> lock(mu_);
  passive_.clear();
  pointer_ = PointerGrab{};
  keyboard_grab_window_ = 0;
  keyboard_grab_fd_ = -1;
}

void GrabTable::removeForWindows(const std::vector<uint32_t>& xids) {
  std::lock_guard<std::mutex> lock(mu_);
  // Remove passive grabs whose grabWindow is in the destroyed set
  passive_.erase(
    std::remove_if(passive_.begin(), passive_.end(),
      [&](const PassiveGrab& g) {
        return std::find(xids.begin(), xids.end(), g.grabWindow) != xids.end();
      }),
    passive_.end());
  // Clear active grab if it references a destroyed window
  if (pointer_.active &&
      std::find(xids.begin(), xids.end(), pointer_.grabWindow) != xids.end()) {
    pointer_ = PointerGrab{};
  }
  // Same for the keyboard grab (was missed — a destroyed grab window left
  // the keyboard grabbed forever; review §6.5)
  if (keyboard_grab_window_ != 0 &&
      std::find(xids.begin(), xids.end(), keyboard_grab_window_) != xids.end()) {
    keyboard_grab_window_ = 0;
    keyboard_grab_fd_ = -1;
  }
}

void GrabTable::clearOwnedBy(int owner_fd) {
  if (owner_fd < 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (pointer_.active && pointer_.owner_fd == owner_fd) {
    pointer_ = PointerGrab{};
  }
  if (keyboard_grab_window_ != 0 && keyboard_grab_fd_ == owner_fd) {
    keyboard_grab_window_ = 0;
    keyboard_grab_fd_ = -1;
  }
}

} // namespace x11
