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
  std::unique_lock<std::shared_mutex> lock(mu_);
  map_[xid] = s;
}

void DrawableSurfaceRegistry::clear(uint32_t xid) {
  if (xid == 0) return;
  std::unique_lock<std::shared_mutex> lock(mu_);
  map_.erase(xid);
}

bool DrawableSurfaceRegistry::get(uint32_t xid, SurfaceDesc& out) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto it = map_.find(xid);
  if (it == map_.end()) return false;
  out = it->second;
  return true;
}

bool DrawableSurfaceRegistry::has(uint32_t xid) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  return map_.find(xid) != map_.end();
}

DrawableSurfaceRegistry::ReadHandle
DrawableSurfaceRegistry::acquireRead(uint32_t xid) const {
  if (xid == 0) return ReadHandle{};

  // Take the shared lock first.  We snapshot the descriptor while holding
  // the lock, then return a handle that retains the lock.  All readers run
  // concurrently; only an exclusive writer (set/clear) blocks.
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto it = map_.find(xid);
  if (it == map_.end() || it->second.ptr == nullptr) {
    // No surface (or entry exists but ptr is null).  Drop the lock by
    // letting it go out of scope; return an invalid handle.
    return ReadHandle{};
  }
  // Move the lock into the handle.  Caller's RAII releases on scope end.
  return ReadHandle(std::move(lock), it->second);
}

} // namespace x11
