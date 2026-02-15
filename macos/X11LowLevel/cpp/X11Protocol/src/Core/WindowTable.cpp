//
//  WindowTable.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/28/26.
//

#include <unordered_map>
#include <algorithm>
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

  
bool WindowTable::exists(uint32_t xid) const {
  if (xid == 0) return false;
  std::lock_guard<std::mutex> lock(mu_);
  return map_.find(xid) != map_.end();
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
  fprintf(stderr,
          "[MASK] xid=0x%08X event_mask=0x%08X\n",
          (unsigned)xid, (unsigned)event_mask);
  
  if (xid == 0) return;

  std::lock_guard<std::mutex> lock(mu_);
  WindowState* st = findLocked(xid);
  if (!st) return;

  st->event_mask = event_mask;
  st->serial++;
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

#ifndef NDEBUG
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
#ifndef NDEBUG
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

#ifndef NDEBUG
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
  if (outParent) *outParent = 0;
  if (outNChildren) *outNChildren = 0;
  if (!outChildren || maxChildren == 0) {
    // still return parent even if no child buffer
    std::lock_guard<std::mutex> lock(mu_);
    const WindowState* st = findLocked(wid);
    if (outParent) *outParent = st ? st->parent : 0;
    return st != nullptr;
  }

  std::lock_guard<std::mutex> lock(mu_);

  // Parent:
  const WindowState* st = findLocked(wid);
  if (!st) {
    // X11 behavior for unknown wid varies; for bring-up, just "not found"
    return false;
  }
  if (outParent) *outParent = st->parent;

  // Children:
  uint32_t n = 0;
  for (const auto& kv : map_) {
    const WindowState& ch = kv.second;
    if (ch.parent == wid) {
      if (n < maxChildren) outChildren[n] = ch.xid;
      n++;
      if (n >= maxChildren) break; // cap like your old code
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

  // BFS over parent links
  std::vector<uint32_t> queue;
  queue.push_back(root);

  while (!queue.empty()) {
    uint32_t p = queue.back();
    queue.pop_back();

    for (const auto& kv : map_) {
      const WindowState& st = kv.second;
      if (st.parent == p) {
        out.push_back(st.xid);
        queue.push_back(st.xid);
      }
    }
  }
  return out;
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
    
} // namespace x11
