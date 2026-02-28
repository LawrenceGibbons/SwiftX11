//
//  GrabTable.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/17/26.
//

#include "Core/GrabTable.hpp"

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

void GrabTable::setPointerGrab(uint32_t grabWindow, bool ownerEvents, uint16_t eventMask) {
  std::lock_guard<std::mutex> lock(mu_);
  pointer_.active = true;
  pointer_.grabWindow = grabWindow;
  pointer_.ownerEvents = ownerEvents;
  pointer_.eventMask = eventMask;
}

void GrabTable::clearPointerGrab() {
  std::lock_guard<std::mutex> lock(mu_);
  pointer_ = PointerGrab{};
}

bool GrabTable::getPointerGrab(PointerGrab& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  out = pointer_;
  return out.active;
}

void GrabTable::clearAll() {
  std::lock_guard<std::mutex> lock(mu_);
  passive_.clear();
  pointer_ = PointerGrab{};
}

} // namespace x11
