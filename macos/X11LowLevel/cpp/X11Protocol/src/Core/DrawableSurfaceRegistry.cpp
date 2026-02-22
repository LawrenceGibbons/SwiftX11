//
//  DrawableSurfaceRegistry.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/22/26.
//

#include "Core/DrawableSurfaceRegistry.hpp"

namespace x11 {

void DrawableSurfaceRegistry::set(uint32_t xid, const SurfaceDesc& s) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  map_[xid] = s;
}

void DrawableSurfaceRegistry::clear(uint32_t xid) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  map_.erase(xid);
}

bool DrawableSurfaceRegistry::get(uint32_t xid, SurfaceDesc& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = map_.find(xid);
  if (it == map_.end()) return false;
  out = it->second;
  return true;
}

bool DrawableSurfaceRegistry::has(uint32_t xid) const {
  std::lock_guard<std::mutex> lock(mu_);
  return map_.find(xid) != map_.end();
}

} // namespace x11
