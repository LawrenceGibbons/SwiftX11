// WindowTable.cpp
#include "WindowTable.hpp"
#include "WindowView.hpp"

namespace x11 {

WindowTable::WindowState* WindowTable::findLocked(uint32_t xid) {
  auto it = map_.find(xid);
  if (it == map_.end()) return nullptr;
  return &it->second;
}

const WindowTable::WindowState* WindowTable::findLocked(uint32_t xid) const {
  auto it = map_.find(xid);
  if (it == map_.end()) return nullptr;
  return &it->second;
}

// Root is XID 0x00000001 in your server.
static constexpr uint32_t kRootXid = 0x00000001u;

uint32_t WindowTable::topLevelAncestorLocked(uint32_t xid) const {
  uint32_t cur = xid;

  for (;;) {
    auto it = map_.find(cur);
    if (it == map_.end()) {
      // If we don't know, fall back to original xid.
      return xid;
    }

    const uint32_t p = it->second.parent;

    // If parent is root/none, cur is top-level.
    if (p == 0 || p == kRootXid) return cur;

    cur = p;
  }
}

void WindowTable::upsert(uint32_t xid, uint32_t parent,
                         int16_t x, int16_t y,
                         uint16_t w, uint16_t h,
                         uint32_t event_mask,
                         int owner_fd)
{
  if (xid == 0) return;
  if (w == 0) w = 1;
  if (h == 0) h = 1;

  std::lock_guard<std::mutex> lock(mu_);
  WindowState& st = map_[xid]; // creates if missing

  st.xid = xid;
  st.parent = parent;
  st.x = x;
  st.y = y;
  st.w = w;
  st.h = h;
  st.event_mask = event_mask;

  // Owner fd is important for sendEvent32 checks.
  st.owner_fd = owner_fd;

  st.serial++;
}

bool WindowTable::erase(uint32_t xid) {
  std::lock_guard<std::mutex> lock(mu_);
  return map_.erase(xid) != 0;
}

void WindowTable::setMapped(uint32_t xid, bool mapped) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->mapped = mapped;
  st->serial++;
}

void WindowTable::setPresentable(uint32_t xid, bool presentable) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->presentable = presentable;
  st->serial++;
}

void WindowTable::markDirty(uint32_t xid) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->dirty = true;
  st->serial++;
}

// ---- The key fix for your “child draws before host is presentable” issue ----
// "ready" is determined by the TOP-LEVEL host, not the child itself.
bool WindowTable::isReadyToPresent(uint32_t xid) const {
  if (xid == 0) return false;

  std::lock_guard<std::mutex> lock(mu_);

  const uint32_t host = topLevelAncestorLocked(xid);
  const WindowState* hs = findLocked(host);
  if (!hs) return false;

  return hs->mapped && hs->presentable;
}

// Consume dirty on the *target xid* if the *host* is ready.
bool WindowTable::consumeDirtyIfReady(uint32_t xid) {
  if (xid == 0) return false;

  std::lock_guard<std::mutex> lock(mu_);

  WindowState* st = findLocked(xid);
  if (!st) return false;

  const uint32_t host = topLevelAncestorLocked(xid);
  const WindowState* hs = findLocked(host);
  if (!hs) return false;

  if (!(hs->mapped && hs->presentable)) return false;

  if (!st->dirty) return false;

  st->dirty = false;
  st->serial++;
  return true;
}

bool WindowTable::snapshot(uint32_t xid, WindowView& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  const WindowState* st = findLocked(xid);
  if (!st) return false;

  out.xid = st->xid;
  out.parent_xid = st->parent;
  out.x = st->x;
  out.y = st->y;
  out.w = st->w;
  out.h = st->h;
  out.event_mask = st->event_mask;
  out.mapped = st->mapped;
  out.presentable = st->presentable;
  out.dirty = st->dirty;
  out.owner_fd = st->owner_fd;
  return true;
}

void WindowTable::debugState(uint32_t xid,
                             uint32_t* out_parent,
                             int* out_mapped,
                             int* out_presentable,
                             int* out_dirty,
                             int* out_owner_fd) const
{
  if (out_parent)      *out_parent = 0;
  if (out_mapped)      *out_mapped = 0;
  if (out_presentable) *out_presentable = 0;
  if (out_dirty)       *out_dirty = 0;
  if (out_owner_fd)    *out_owner_fd = -1;

  std::lock_guard<std::mutex> lock(mu_);
  const WindowState* st = findLocked(xid);
  if (!st) return;

  if (out_parent)      *out_parent = st->parent;
  if (out_mapped)      *out_mapped = st->mapped ? 1 : 0;
  if (out_presentable) *out_presentable = st->presentable ? 1 : 0;
  if (out_dirty)       *out_dirty = st->dirty ? 1 : 0;
  if (out_owner_fd)    *out_owner_fd = st->owner_fd;
}

  
// WindowTable.cpp

void WindowTable::setEventMask(uint32_t xid, uint32_t event_mask) {
  if (xid == 0) return;

  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;

  st->event_mask = event_mask;
  st->serial++;
}

void WindowTable::setGeometry(uint32_t xid,
                              int16_t x, int16_t y,
                              uint16_t w, uint16_t h)
{
  if (xid == 0) return;
  if (w == 0) w = 1;
  if (h == 0) h = 1;

  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;

  fprintf(stderr, "[BRIDGE] set_geometry xid=0x%08X mask=0x%08X x=%d y=%d w=%d h=%d mapped=%d presentable=%d dirty=%d\n", 
          xid, st->event_mask, x, y, (int)w, (int)h, st->mapped, st->presentable, st->dirty);

  st->x = x;
  st->y = y;
  st->w = w;
  st->h = h;

  // Do NOT touch st->dirty here.
  // st->dirty is for “damage happened before ready-to-present” gating only.
  // Geometry changes are handled by X11 Configure/Expose notifications + explicit enqueue_damage_window in C.  //st->dirty = true;
  st->serial++;
}
  
  
} // namespace x11
