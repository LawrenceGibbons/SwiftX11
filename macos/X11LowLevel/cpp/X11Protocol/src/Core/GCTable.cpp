//
//  GCTable.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/27/26.
//

#include "GCTable.hpp"

namespace x11 {

GCTable& GCTable::instance() {
  static GCTable t;
  return t;
}

GCState GCTable::getOrCreate(uint32_t gcXid) {
  if (gcXid == 0) return GCState{};
  std::lock_guard<std::mutex> lock(mu_);
  auto it = map_.find(gcXid);
  if (it != map_.end()) return it->second;

  GCState st;
  st.xid = gcXid;
  map_[gcXid] = st;
  return st;
}

bool GCTable::find(uint32_t gcXid, GCState& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = map_.find(gcXid);
  if (it == map_.end()) return false;
  out = it->second;
  return true;
}

void GCTable::upsert(const GCState& st) {
  if (st.xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  map_[st.xid] = st;
}

void GCTable::erase(uint32_t gcXid) {
  if (gcXid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  map_.erase(gcXid);
}

} // namespace x11
