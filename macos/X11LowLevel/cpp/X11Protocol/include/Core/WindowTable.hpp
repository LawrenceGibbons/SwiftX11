//
//  WindowTable.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/25/26.
//

#pragma once

#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace x11 {

struct WindowView;

class WindowTable {
public:
  WindowTable() = default;

  // Insert or update a window record.
  void upsert(uint32_t xid, uint32_t parent,
              int16_t x, int16_t y,
              uint16_t w, uint16_t h,
              uint32_t event_mask,
              int owner_fd);

  bool erase(uint32_t xid);

  bool exists(uint32_t xid) const;
  
  void setMapped(uint32_t xid, bool mapped);
  void setPresentable(uint32_t xid, bool presentable);
  void markDirty(uint32_t xid);

  bool isReadyToPresent(uint32_t xid) const;
  bool consumeDirtyIfReady(uint32_t xid);

  bool snapshot(uint32_t xid, WindowView& out) const;
  
  // Update only the event mask (CWEventMask / SelectInput semantics)
  void setEventMask(uint32_t xid, uint32_t event_mask);

  // Update geometry without reallocating framebuffers
  // (framebuffer resizing is handled elsewhere)
  void setGeometry(uint32_t xid,
                   int16_t x, int16_t y,
                   uint16_t w, uint16_t h);
  
  void setGeometryDbg(uint32_t xid, 
                      int16_t x, int16_t y,
                      uint16_t w, uint16_t h,
                      const char* why, const char* file, int line);

  
#define WT_GEOM_SET(xid,x,y,w,h,why) \
  ctx.windows().setGeometryDbg((xid),(x),(y),(w),(h),(why),__FILE__,__LINE__)

#define WT_NOTE_FB_RESIZE(xid,why) \
  ctx.windows().noteFbResizeDbg((xid),(why),__FILE__,__LINE__)
  
  // Rootless host geometry update: update host geometry AND clamp descendants
  // to fit within their direct parent. Does NOT set dirty (present gating) flags.
  void setGeometryRootlessHost(uint32_t xid, int16_t x, int16_t y, uint16_t w, uint16_t h);
  
  void noteFbResizeDbg(uint32_t xid,
                       const char* why,
                       const char* file,
                       int line);
  
  // “Top-level” in rootless world = parent chain ends at 1 (root). Return the highest non-root window.
  uint32_t topLevelAncestorOf(uint32_t xid) const;
  
  // Erase all windows owned by owner_fd.
  // Returns erased XIDs in child-first order (deepest children first).
  std::vector<uint32_t> eraseOwnedBy(int owner_fd);

  // Debug snapshot directly from WindowState (no WindowView required)
  void debugState(uint32_t xid,
                  uint32_t* out_parent,
                  int* out_mapped,
                  int* out_presentable,
                  int* out_dirty,
                  int* out_owner_fd) const;
  
  // QueryTree helper (core X11 major=15)
  bool queryTree(uint32_t wid,
                 uint32_t* outParent,
                 uint32_t* outChildren,
                 uint32_t  maxChildren,
                 uint32_t* outNChildren) const;

  // tracking subwindows
  std::vector<uint32_t> descendantsOf(uint32_t root) const;
  
  // Bring-up behavior: keep each descendant's x/y (relative to parent) unchanged,
  // but clamp w/h so the child fits within its *direct* parent’s bounds.
  // Marks dirty when a child's size changes.
  void clampDescendantsToParent(uint32_t rootXid);
  

private:
  struct WindowState {
    uint32_t xid = 0;
    uint32_t parent = 0;
    int16_t  x = 0;
    int16_t  y = 0;
    uint16_t w = 1;
    uint16_t h = 1;

    uint32_t event_mask = 0;
    bool mapped = false;
    bool presentable = false;
    bool dirty = false;

    int owner_fd = -1;

    uint64_t serial = 0;
    
    // debug breadcrumbs
    const char* lastGeomWhy = nullptr;
    const char* lastGeomFile = nullptr;
    int         lastGeomLine = 0;

    const char* lastFbWhy = nullptr;
    const char* lastFbFile = nullptr;
    int         lastFbLine = 0;

  };

  mutable std::mutex mu_;
  std::unordered_map<uint32_t, WindowState> map_;

  // Helpers (must be called with mu_ held)
  WindowState* findLocked(uint32_t xid);
  const WindowState* findLocked(uint32_t xid) const;
  uint32_t topLevelAncestorLocked(uint32_t xid) const;
  
  void setGeomLocked_(WindowState& st,
                      int16_t x, int16_t y,
                      uint16_t w, uint16_t h,
                      const char* why,
                      const char* file,
                      int line);
  
  void setXYLocked_(WindowState& st,
                    int16_t x, int16_t y,
                    const char* why,
                    const char* file,
                    int line);
};

} // namespace x11
