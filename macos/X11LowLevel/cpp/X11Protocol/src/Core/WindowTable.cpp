//
//  WindowTable.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/28/26.
//

#include <unordered_map>
#include <algorithm>
#include <deque>
#include <functional>

#include "Core/WindowTable.hpp"
#include "Core/WindowView.hpp"

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


  uint32_t WindowTable::topLevelAncestorOf(uint32_t xid) const {
    if (xid == 0) return 0;
    std::lock_guard<std::mutex> lock(mu_);
    return topLevelAncestorLocked(xid);
  }

  uint32_t WindowTable::topLevelAncestorLocked(uint32_t xid) const {
    // Root is XID 0x00000001 in your server.
    static constexpr uint32_t kRootXid = 0x00000001u;
    uint32_t cur = xid;

    for (;;) {
      auto it = map_.find(cur);
      if (it == map_.end()) {
        // If we don't know the chain, fall back to the original xid.
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

  // Track old parent for children_order_ bookkeeping
  const uint32_t oldParent = st.parent;

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

  // Maintain children_order_: append to new parent's child list.
  // (For a fresh insert, oldParent == 0 so no removal needed.)
  if (parent != 0) {
    if (oldParent != parent && oldParent != 0) {
      // Remove from old parent's child list (reparent via upsert)
      auto& oldList = children_order_[oldParent];
      oldList.erase(std::remove(oldList.begin(), oldList.end(), xid), oldList.end());
    }
    auto& newList = children_order_[parent];
    // Only append if not already present (avoid duplicates on upsert update)
    if (std::find(newList.begin(), newList.end(), xid) == newList.end()) {
      newList.push_back(xid);
    }
  }
}

  
bool WindowTable::exists(uint32_t xid) const {
  if (xid == 0) return false;
  std::lock_guard<std::mutex> lock(mu_);
  return map_.find(xid) != map_.end();
}  
  
  
  
bool WindowTable::erase(uint32_t xid) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = map_.find(xid);
  if (it == map_.end()) return false;

  // Remove from parent's children_order_ list
  const uint32_t parent = it->second.parent;
  if (parent != 0) {
    auto pit = children_order_.find(parent);
    if (pit != children_order_.end()) {
      auto& list = pit->second;
      list.erase(std::remove(list.begin(), list.end(), xid), list.end());
    }
  }
  // Remove this window's own children list
  children_order_.erase(xid);

  map_.erase(it);
  return true;
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

void WindowTable::setSurfaceResizeExposed(uint32_t xid, bool v) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->surface_resize_exposed = v;
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

bool WindowTable::absoluteOffsetInHost(uint32_t host, uint32_t xid,
                                       int32_t& outX, int32_t& outY) const
{
  outX = 0;
  outY = 0;
  if (host == 0 || xid == 0) return false;
  if (host == xid) return true;

  std::lock_guard<std::mutex> lock(mu_);

  uint32_t cur = xid;
  for (int hop = 0; hop < 256; hop++) {
    const WindowState* st = findLocked(cur);
    if (!st) return false;

    // In X11, (x,y) is the outer border corner; the drawable starts at
    // (x + border_width, y + border_width) in parent coords.
    outX += (int32_t)st->x + (int32_t)st->border_width;
    outY += (int32_t)st->y + (int32_t)st->border_width;

    if (st->parent == 0) return false;
    if (st->parent == host) return true;

    cur = st->parent;
  }
  return false;
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
  out.xi2_mask = st->xi2_mask;
  out.background_pixel = st->background_pixel;
  out.has_background_pixel = st->has_background_pixel;
  out.is_parent_relative = st->is_parent_relative;
  out.background_pixmap = st->background_pixmap;
  out.mapped = st->mapped;
  out.presentable = st->presentable;
  out.dirty = st->dirty;
  out.surface_resize_exposed = st->surface_resize_exposed;
  out.owner_fd = st->owner_fd;
  out.border_width = st->border_width;
  out.border_pixel = st->border_pixel;
  out.override_redirect = st->override_redirect;
  out.win_gravity = st->win_gravity;
  out.bit_gravity = st->bit_gravity;
  out.backing_store = st->backing_store;
  out.wants_input = st->wants_input;
  out.wants_take_focus = st->wants_take_focus;
  out.bounding_shaped = st->shape_bounding.shaped;
  out.clip_shaped = st->shape_clip.shaped;
  out.input_shaped = st->shape_input.shaped;
  return true;
}

void WindowTable::setBackgroundPixel(uint32_t xid, uint32_t pixel_argb) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
#ifndef NDEBUG
  if (st->background_pixmap != 0) {
    fprintf(stderr, "[BG_SET] xid=0x%08X setBackgroundPixel=0x%08X (was pixmap=0x%08X)\n",
            (unsigned)xid, (unsigned)pixel_argb, (unsigned)st->background_pixmap);
  }
#endif
  st->background_pixel = pixel_argb;
  st->has_background_pixel = true;
  st->is_parent_relative = false;  // explicit pixel overrides ParentRelative
  st->background_pixmap = 0;       // pixel overrides pixmap
  st->serial++;
}

void WindowTable::setBackgroundPixmap(uint32_t xid, uint32_t pixmap_xid) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
#ifndef NDEBUG
  fprintf(stderr, "[BG_SET] xid=0x%08X setBackgroundPixmap=0x%08X (was pixel=0x%08X has=%d)\n",
          (unsigned)xid, (unsigned)pixmap_xid,
          (unsigned)st->background_pixel, (int)st->has_background_pixel);
#endif
  st->background_pixmap = pixmap_xid;
  st->has_background_pixel = false;  // pixmap overrides solid
  st->is_parent_relative = false;
  st->background_pixel = 0;
  st->serial++;
}

void WindowTable::clearBackground(uint32_t xid) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->background_pixel = 0;
  st->has_background_pixel = false;
  st->is_parent_relative = false;
  st->background_pixmap = 0;
  st->serial++;
}

void WindowTable::setParentRelative(uint32_t xid) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->is_parent_relative = true;
  st->background_pixmap = 0;  // ParentRelative overrides pixmap
  st->serial++;
}

bool WindowTable::resolveParentRelativeBackground(uint32_t xid) {
  if (xid == 0) return false;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return false;

  st->is_parent_relative = true;

  // Walk up parent chain to find nearest ancestor with has_background_pixel
  // that isn't itself ParentRelative. This gives us a cached value for the
  // common case, but resolveBackgroundForClear() also does a live walk.
  uint32_t cur = st->parent;
  for (int depth = 0; depth < 64 && cur != 0 && cur != 1; depth++) {
    const WindowState* parent = findLocked(cur);
    if (!parent) break;
    if (parent->has_background_pixel && !parent->is_parent_relative) {
      st->background_pixel = parent->background_pixel;
      st->has_background_pixel = true;
      st->serial++;
      return true;
    }
    // If parent is also ParentRelative, keep walking
    if (parent->has_background_pixel) {
      // Parent resolved its ParentRelative — use that value
      st->background_pixel = parent->background_pixel;
      st->has_background_pixel = true;
      st->serial++;
      return true;
    }
    cur = parent->parent;
  }
  return false;
}

bool WindowTable::resolveBackgroundForClear(uint32_t xid, uint32_t& out_pixel) const {
  if (xid == 0) return false;
  std::lock_guard<std::mutex> lock(mu_);
  const WindowState* st = findLocked(xid);
  if (!st) return false;

  // If the window has an explicit background pixel (not from ParentRelative),
  // use it directly.
  if (st->has_background_pixel && !st->is_parent_relative) {
    out_pixel = st->background_pixel;
    return true;
  }

  // If ParentRelative, dynamically walk the parent chain to find the
  // current background. This handles the case where the parent's background
  // was set after this window was created.
  if (st->is_parent_relative) {
    uint32_t cur = st->parent;
    for (int depth = 0; depth < 64 && cur != 0 && cur != 1; depth++) {
      const WindowState* anc = findLocked(cur);
      if (!anc) break;
      if (anc->has_background_pixel) {
        out_pixel = anc->background_pixel;
        return true;
      }
      cur = anc->parent;
    }
    // ParentRelative but no ancestor has a background — fall through
  }

  // If the window has a cached background_pixel (from earlier resolution), use it.
  if (st->has_background_pixel) {
    out_pixel = st->background_pixel;
    return true;
  }

  // No background defined (CWBackPixmap=None or never set).
  // Per X11 spec, ClearArea should not modify window contents.
  return false;
}

bool WindowTable::resolveBackgroundPixmapForClear(uint32_t xid, uint32_t& out_pixmap) const {
  if (xid == 0) return false;
  std::lock_guard<std::mutex> lock(mu_);
  const WindowState* st = findLocked(xid);
  if (!st) return false;
  if (st->background_pixmap != 0) {
    out_pixmap = st->background_pixmap;
    return true;
  }
  return false;
}

void WindowTable::setBorderWidth(uint32_t xid, uint16_t bw) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->border_width = bw;
  st->serial++;
}

void WindowTable::setBorderPixel(uint32_t xid, uint32_t pixel_argb) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->border_pixel = pixel_argb;
  st->serial++;
}

// Window manager attribute setters
void WindowTable::setOverrideRedirect(uint32_t xid, bool v) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->override_redirect = v;
  st->serial++;
}

void WindowTable::setWinGravity(uint32_t xid, uint8_t v) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->win_gravity = v;
  st->serial++;
}

void WindowTable::setBitGravity(uint32_t xid, uint8_t v) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->bit_gravity = v;
  st->serial++;
}

void WindowTable::setBackingStore(uint32_t xid, uint8_t v) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->backing_store = v;
  st->serial++;
}

void WindowTable::setWantsInput(uint32_t xid, bool v) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->wants_input = v;
  st->serial++;
}

void WindowTable::setWantsTakeFocus(uint32_t xid, bool v) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->wants_take_focus = v;
  st->serial++;
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
#ifdef X11_TRACE_VERBOSE
  fprintf(stderr,
          "[MASK] xid=0x%08X event_mask=0x%08X\n",
          (unsigned)xid, (unsigned)event_mask);
#endif
  
  if (xid == 0) return;

  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;

  st->event_mask = event_mask;
  st->serial++;
}

void WindowTable::setXI2Mask(uint32_t xid, uint32_t xi2_mask) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->xi2_mask = xi2_mask;
  st->serial++;
#ifndef NDEBUG
  fprintf(stderr, "[XI2_MASK] xid=0x%08X xi2_mask=0x%08X\n",
          (unsigned)xid, (unsigned)xi2_mask);
#endif
}

// Assumes mu_ is held.
void WindowTable::setGeomLocked_(WindowState& st,
                                int16_t x, int16_t y,
                                uint16_t w, uint16_t h,
                                const char* why,
                                const char* file,
                                int line)
{
  if (w == 0) w = 1;
  if (h == 0) h = 1;

  const uint16_t oldW = st.w;
  const uint16_t oldH = st.h;

#ifdef X11_TRACE_VERBOSE
  fprintf(stderr,
          "[WT] setGeometry xid=0x%08X old=(%d,%d %ux%u) new=(%d,%d %ux%u) why=%s @%s:%d\n",
          (unsigned)st.xid,
          (int)st.x, (int)st.y, (unsigned)st.w, (unsigned)st.h,
          (int)x, (int)y, (unsigned)w, (unsigned)h,
          why ? why : "?", file ? file : "?", line);
#endif

  st.x = x;
  st.y = y;
  st.w = w;
  st.h = h;

  // IMPORTANT: only stamp lastGeom* when SIZE changes (keeps clampXY from stomping it)
  if (oldW != w || oldH != h) {
    st.lastGeomWhy  = why;
    st.lastGeomFile = file;
    st.lastGeomLine = line;
#ifdef X11_TRACE_VERBOSE
    fprintf(stderr,
            "[WT] setGeometry SIZE xid=0x%08X parent=0x%08X old=%ux%u new=%ux%u why=%s @%s:%d\n",
            (unsigned)st.xid,
            (unsigned)st.parent,
            (unsigned)oldW, (unsigned)oldH,
            (unsigned)w, (unsigned)h,
            why ? why : "?", file ? file : "?", line);
#endif
  }

  st.serial++;
}

// Assumes mu_ is held. Does NOT touch lastGeom* (position-only changes).
void WindowTable::setXYLocked_(WindowState& st,
                               int16_t x, int16_t y,
                               const char* why,
                               const char* file,
                               int line)
{
  if (x == st.x && y == st.y) return;

#ifndef NDEBUG
  fprintf(stderr,
          "[WT] clampXY xid=0x%08X old=(%d,%d) new=(%d,%d) why=%s @%s:%d\n",
          (unsigned)st.xid,
          (int)st.x, (int)st.y,
          (int)x, (int)y,
          why ? why : "?", file ? file : "?", line);
#endif

  st.x = x;
  st.y = y;
  st.serial++;
}
void WindowTable::setGeometry(uint32_t xid,
                              int16_t x, int16_t y,
                              uint16_t w, uint16_t h)
{
  setGeometryDbg(xid, x, y, w, h, "setGeometry", __FILE__, __LINE__);
}
  
void WindowTable::setGeometryDbg(uint32_t xid,
                                 int16_t x, int16_t y,
                                 uint16_t w, uint16_t h,
                                 const char* why,
                                 const char* file,
                                 int line)
{
  if (xid == 0) return;

  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;

  setGeomLocked_(*st, x, y, w, h, why, file, line);
}

  
void WindowTable::setGeometryRootlessHost(uint32_t xid,
                                          int16_t x, int16_t y,
                                          uint16_t w, uint16_t h)
{
  // First: set host geometry
  setGeometryDbg(xid, x, y, w, h, "RootlessHost:setGeometry", __FILE__, __LINE__);
//  setGeometry(xid, x, y, w, h);

  // Then: enforce rootless constraint policy
  clampDescendantsToParent(xid);
}
  
  
void WindowTable::noteFbResizeDbg(uint32_t xid,
                                 const char* why,
                                 const char* file,
                                 int line)
{
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;

  st->lastFbWhy  = why;
  st->lastFbFile = file;
  st->lastFbLine = line;

#ifdef X11_TRACE_VERBOSE
  fprintf(stderr, "[WT] noteFbResize xid=0x%08X why=%s @%s:%d\n",
          (unsigned)xid, why ? why : "?", file ? file : "?", line);
#endif
}  
  
bool WindowTable::queryTree(uint32_t wid,
                            uint32_t* outParent,
                            uint32_t* outChildren,
                            uint32_t  maxChildren,
                            uint32_t* outNChildren) const
{
  static constexpr uint32_t kRoot = 0x00000001u;

  if (outParent) *outParent = 0;
  if (outNChildren) *outNChildren = 0;
  if (!outChildren || maxChildren == 0) {
    // still return parent even if no child buffer
    std::lock_guard<std::mutex> lock(mu_);
    const WindowState* st = findLocked(wid);
    if (outParent) *outParent = st ? st->parent : 0;
    return st != nullptr || wid == kRoot;
  }

  std::lock_guard<std::mutex> lock(mu_);

  // Parent:
  const WindowState* st = findLocked(wid);
  if (!st && wid != kRoot) {
    // Unknown window — return error
    return false;
  }
  if (outParent) *outParent = st ? st->parent : 0;  // root has no parent

  // Children: return in stacking order (bottom-to-top = creation order)
  uint32_t n = 0;
  auto cit = children_order_.find(wid);
  if (cit != children_order_.end()) {
    for (uint32_t child : cit->second) {
      if (map_.find(child) == map_.end()) continue; // stale entry
      if (n < maxChildren) outChildren[n] = child;
      n++;
      if (n >= maxChildren) break;
    }
  }

  if (outNChildren) *outNChildren = n;
  return true;
}
  
  

std::vector<uint32_t> WindowTable::descendantsOf(uint32_t root) const {
  std::vector<uint32_t> out;
  if (root == 0) return out;

  std::lock_guard<std::mutex> lock(mu_);
  if (map_.find(root) == map_.end()) return out;

  // BFS using children_order_ for stacking-aware ordering (bottom-to-top).
  // This ensures lower-stacking children appear before higher-stacking ones.
  std::deque<uint32_t> queue;
  queue.push_back(root);

  while (!queue.empty()) {
    uint32_t p = queue.front();
    queue.pop_front();

    auto cit = children_order_.find(p);
    if (cit != children_order_.end()) {
      for (uint32_t child : cit->second) {
        // Verify child still exists in map
        if (map_.find(child) != map_.end()) {
          out.push_back(child);
          queue.push_back(child);
        }
      }
    }
  }
  return out;
}

std::vector<uint32_t> WindowTable::childrenInStackOrder(uint32_t parent) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto cit = children_order_.find(parent);
  if (cit == children_order_.end()) return {};
  // Return only children that still exist
  std::vector<uint32_t> result;
  for (uint32_t child : cit->second) {
    if (map_.find(child) != map_.end()) {
      result.push_back(child);
    }
  }
  return result;
}
  
  std::vector<uint32_t> WindowTable::eraseOwnedBy(int owner_fd)
  {
    std::vector<uint32_t> xids;
    std::unordered_map<uint32_t, uint32_t> parentOf;
    parentOf.reserve(256);

    // 1) Snapshot owned windows and erase them under lock.
    {
      std::lock_guard<std::mutex> lock(mu_);

      for (const auto& kv : map_) {
        const uint32_t xid = kv.first;
        const WindowState& st = kv.second;
        if (st.owner_fd == owner_fd) {
          xids.push_back(xid);
          parentOf[xid] = st.parent;
        }
      }

      for (uint32_t xid : xids) {
        // Clean up children_order_: remove from parent's list and remove own list
        auto pit = parentOf.find(xid);
        if (pit != parentOf.end() && pit->second != 0) {
          auto cit = children_order_.find(pit->second);
          if (cit != children_order_.end()) {
            auto& list = cit->second;
            list.erase(std::remove(list.begin(), list.end(), xid), list.end());
          }
        }
        children_order_.erase(xid);
        map_.erase(xid);
      }
    }

    // 2) Sort child-first among the owned set (important for Swift view teardown).
    std::unordered_map<uint32_t, int> memo;
    memo.reserve(xids.size());

    std::function<int(uint32_t)> depth = [&](uint32_t xid) -> int {
      auto it = memo.find(xid);
      if (it != memo.end()) return it->second;

      int d = 0;
      auto pIt = parentOf.find(xid);
      if (pIt != parentOf.end()) {
        const uint32_t p = pIt->second;
        if (parentOf.find(p) != parentOf.end()) {
          d = 1 + depth(p);
        }
      }
      memo[xid] = d;
      return d;
    };

    std::stable_sort(xids.begin(), xids.end(),
                     [&](uint32_t a, uint32_t b) { return depth(a) > depth(b); });

    return xids;
  }
  
  
  void WindowTable::clampDescendantsToParent(uint32_t rootXid)
  {
    if (rootXid == 0) return;

    std::lock_guard<std::mutex> lock(mu_);
    auto itRoot = map_.find(rootXid);
    if (itRoot == map_.end()) return;

    // BFS from rootXid (direct-parent clamping)
    std::vector<uint32_t> queue;
    queue.push_back(rootXid);

    while (!queue.empty()) {
      const uint32_t parentXid = queue.back();
      queue.pop_back();

      auto itP = map_.find(parentXid);
      if (itP == map_.end()) continue;
      const WindowState& p = itP->second;

      // Clamp children of this parent.
      for (auto& kv : map_) {
        WindowState& c = kv.second;
        if (c.parent != parentXid) continue;

        // Enqueue this child so we clamp its children too
        queue.push_back(c.xid);

        // ---- Option B: Clamp x/y only; DO NOT modify c.w/c.h ----
        // Keep child origin inside parent. If parent is 1px wide/high, origin must be 0.
        const int32_t maxX = (p.w > 0) ? ((int32_t)p.w - 1) : 0;
        const int32_t maxY = (p.h > 0) ? ((int32_t)p.h - 1) : 0;

        int32_t nx = (int32_t)c.x;
        int32_t ny = (int32_t)c.y;

        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx > maxX) nx = maxX;
        if (ny > maxY) ny = maxY;

        const int16_t newX = (int16_t)nx;
        const int16_t newY = (int16_t)ny;

        if (newX != c.x || newY != c.y) {
          setXYLocked_(c, newX, newY, "ClampDescendantsToParent", __FILE__, __LINE__);
        }
      }
    }
  }

  
  
void WindowTable::setCursor(uint32_t xid, uint32_t cursor_xid) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  st->cursor_xid = cursor_xid;
  st->serial++;
#ifdef X11_TRACE_VERBOSE
  fprintf(stderr, "[WT] setCursor xid=0x%08X cursor=0x%08X\n",
          (unsigned)xid, (unsigned)cursor_xid);
#endif
}  
  
uint32_t WindowTable::cursor(uint32_t xid) const {
  if (xid == 0) return 0;

  // Root is XID 0x00000001 in your server.
  static constexpr uint32_t kRootXid = 0x00000001u;

  std::lock_guard<std::mutex> lock(mu_);

  uint32_t cur = xid;
  int safety = 0;

  while (cur != 0 && cur != kRootXid) {
    const WindowState* st = findLocked(cur);
    if (!st) return 0;                 // unknown chain => default

    if (st->cursor_xid != 0) {
      return st->cursor_xid;           // explicit cursor set here
    }

    cur = st->parent;                  // inherit from parent
    if (++safety > 64) return 0;       // defensive against cycles
  }

  // Root or none: no explicit cursor
  return 0;
}
  
// ---- Stacking order manipulation ----

void WindowTable::raiseToTop(uint32_t xid) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  const uint32_t parent = st->parent;
  if (parent == 0) return;

  auto cit = children_order_.find(parent);
  if (cit == children_order_.end()) return;
  auto& list = cit->second;
  list.erase(std::remove(list.begin(), list.end(), xid), list.end());
  list.push_back(xid);  // top of stack (last = highest)
  st->serial++;
}

void WindowTable::lowerToBottom(uint32_t xid) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  const uint32_t parent = st->parent;
  if (parent == 0) return;

  auto cit = children_order_.find(parent);
  if (cit == children_order_.end()) return;
  auto& list = cit->second;
  list.erase(std::remove(list.begin(), list.end(), xid), list.end());
  list.insert(list.begin(), xid);  // bottom of stack (first = lowest)
  st->serial++;
}

void WindowTable::restackAbove(uint32_t xid, uint32_t sibling) {
  if (xid == 0 || sibling == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  const uint32_t parent = st->parent;
  if (parent == 0) return;

  auto cit = children_order_.find(parent);
  if (cit == children_order_.end()) return;
  auto& list = cit->second;

  // Remove xid first
  list.erase(std::remove(list.begin(), list.end(), xid), list.end());

  // Find sibling and insert just after it (= just above in stacking order)
  auto it = std::find(list.begin(), list.end(), sibling);
  if (it != list.end()) {
    list.insert(it + 1, xid);
  } else {
    list.push_back(xid);  // sibling not found → place at top
  }
  st->serial++;
}

void WindowTable::restackBelow(uint32_t xid, uint32_t sibling) {
  if (xid == 0 || sibling == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;
  const uint32_t parent = st->parent;
  if (parent == 0) return;

  auto cit = children_order_.find(parent);
  if (cit == children_order_.end()) return;
  auto& list = cit->second;

  // Remove xid first
  list.erase(std::remove(list.begin(), list.end(), xid), list.end());

  // Find sibling and insert just before it (= just below in stacking order)
  auto it = std::find(list.begin(), list.end(), sibling);
  if (it != list.end()) {
    list.insert(it, xid);
  } else {
    list.insert(list.begin(), xid);  // sibling not found → place at bottom
  }
  st->serial++;
}

bool WindowTable::reparent(uint32_t xid, uint32_t newParent, int16_t x, int16_t y) {
  if (xid == 0) return false;
  std::lock_guard<std::mutex> lock(mu_);
  auto it = map_.find(xid);
  if (it == map_.end()) return false;

  const uint32_t oldParent = it->second.parent;

  // Update children_order_: remove from old parent, add to new parent
  if (oldParent != 0 && oldParent != newParent) {
    auto pit = children_order_.find(oldParent);
    if (pit != children_order_.end()) {
      auto& list = pit->second;
      list.erase(std::remove(list.begin(), list.end(), xid), list.end());
    }
  }
  if (newParent != 0 && newParent != oldParent) {
    auto& newList = children_order_[newParent];
    if (std::find(newList.begin(), newList.end(), xid) == newList.end()) {
      newList.push_back(xid); // append at top of stacking order
    }
  }

  it->second.parent = newParent;
  it->second.x = x;
  it->second.y = y;
  it->second.serial++;
  return true;
}

// ---------------------------------------------------------------------------
// SHAPE extension
// ---------------------------------------------------------------------------
void WindowTable::setShapeBounding(uint32_t xid, ShapeRegion region) {
  std::lock_guard<std::mutex> lock(mu_);
  auto* st = findLocked(xid);
  if (st) st->shape_bounding = std::move(region);
}

void WindowTable::setShapeClip(uint32_t xid, ShapeRegion region) {
  std::lock_guard<std::mutex> lock(mu_);
  auto* st = findLocked(xid);
  if (st) st->shape_clip = std::move(region);
}

void WindowTable::setShapeInput(uint32_t xid, ShapeRegion region) {
  std::lock_guard<std::mutex> lock(mu_);
  auto* st = findLocked(xid);
  if (st) st->shape_input = std::move(region);
}

ShapeRegion WindowTable::shapeBounding(uint32_t xid) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto* st = findLocked(xid);
  if (!st) return {};
  return st->shape_bounding;
}

ShapeRegion WindowTable::shapeInput(uint32_t xid) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto* st = findLocked(xid);
  if (!st) return {};
  return st->shape_input;
}

bool WindowTable::isShapedBounding(uint32_t xid) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto* st = findLocked(xid);
  return st && st->shape_bounding.shaped;
}

bool WindowTable::isInShapeRegion(uint32_t xid, int16_t lx, int16_t ly) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto* st = findLocked(xid);
  if (!st) return true;  // unknown window — treat as unshaped
  // Prefer input shape, fall back to bounding shape
  if (st->shape_input.shaped)
    return st->shape_input.contains(lx, ly);
  if (st->shape_bounding.shaped)
    return st->shape_bounding.contains(lx, ly);
  return true;  // unshaped
}

} // namespace x11
