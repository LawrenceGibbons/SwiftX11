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

uint8_t GrabTable::tryPointerGrab(const PointerGrab& req) {
  std::lock_guard<std::mutex> lock(mu_);
  if (pointer_.active) {
    // xorg GrabDevice (dix/events.c:5253-5256): a grab of the other level
    // (CORE vs XI2) or held by another client → AlreadyGrabbed.
    if (pointer_.is_xi2 != req.is_xi2) return kAlreadyGrabbed;
    if (pointer_.owner_fd >= 0 && pointer_.owner_fd != req.owner_fd) return kAlreadyGrabbed;
  }
  pointer_ = req;
  pointer_.active = true;
  return kGrabSuccess;
}

uint8_t GrabTable::tryPointerGrab(uint32_t grabWindow, bool ownerEvents,
                                  uint16_t eventMask, int owner_fd,
                                  uint32_t time) {
  PointerGrab req{};
  req.grabWindow  = grabWindow;
  req.ownerEvents = ownerEvents;
  req.eventMask   = eventMask;
  req.owner_fd    = owner_fd;
  req.grab_time   = time;
  req.is_xi2      = false;
  return tryPointerGrab(req);
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

void GrabTable::updatePointerGrabCursor(uint32_t cursor) {
  std::lock_guard<std::mutex> lock(mu_);
  if (pointer_.active) {
    pointer_.cursor = cursor;
  }
}

uint8_t GrabTable::tryKeyboardGrab(const KeyboardGrab& req) {
  std::lock_guard<std::mutex> lock(mu_);
  if (keyboard_.active) {
    if (keyboard_.is_xi2 != req.is_xi2) return kAlreadyGrabbed;
    if (keyboard_.owner_fd >= 0 && keyboard_.owner_fd != req.owner_fd) return kAlreadyGrabbed;
  }
  keyboard_ = req;
  keyboard_.active = true;
  return kGrabSuccess;
}

uint8_t GrabTable::tryKeyboardGrab(uint32_t grabWindow, int owner_fd) {
  KeyboardGrab req{};
  req.grabWindow = grabWindow;
  req.owner_fd   = owner_fd;
  req.is_xi2     = false;
  return tryKeyboardGrab(req);
}

uint32_t GrabTable::clearKeyboardGrab(int owner_fd) {
  std::lock_guard<std::mutex> lock(mu_);
  if (owner_fd >= 0 && keyboard_.active &&
      keyboard_.owner_fd >= 0 && keyboard_.owner_fd != owner_fd) {
    return 0; // another client's grab
  }
  const uint32_t prev = keyboard_.active ? keyboard_.grabWindow : 0;
  keyboard_ = KeyboardGrab{};
  return prev;
}

uint32_t GrabTable::getKeyboardGrab() const {
  std::lock_guard<std::mutex> lock(mu_);
  return keyboard_.active ? keyboard_.grabWindow : 0;
}

bool GrabTable::getKeyboardGrabInfo(KeyboardGrab& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  out = keyboard_;
  return out.active;
}

void GrabTable::clearAll() {
  std::lock_guard<std::mutex> lock(mu_);
  passive_.clear();
  pointer_ = PointerGrab{};
  keyboard_ = KeyboardGrab{};
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
  if (keyboard_.active &&
      std::find(xids.begin(), xids.end(), keyboard_.grabWindow) != xids.end()) {
    keyboard_ = KeyboardGrab{};
  }
}

void GrabTable::clearOwnedBy(int owner_fd) {
  if (owner_fd < 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  if (pointer_.active && pointer_.owner_fd == owner_fd) {
    pointer_ = PointerGrab{};
  }
  if (keyboard_.active && keyboard_.owner_fd == owner_fd) {
    keyboard_ = KeyboardGrab{};
  }
}

} // namespace x11
