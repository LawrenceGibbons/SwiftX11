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
#include <Core/ShapeRegion.hpp>

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

  // Compute the absolute (host-surface-space) offset of 'xid' within 'host'.
  // Returns false if the chain is broken.  Does NOT clamp negatives.
  bool absoluteOffsetInHost(uint32_t host, uint32_t xid,
                            int32_t& outX, int32_t& outY) const;

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

  // tracking subwindows (returns children in bottom-to-top stacking order)
  std::vector<uint32_t> descendantsOf(uint32_t root) const;

  // Direct children in stacking order (bottom-to-top = creation order)
  std::vector<uint32_t> childrenInStackOrder(uint32_t parent) const;
  
  
  // background pixel
  void setBackgroundPixel(uint32_t xid, uint32_t pixel_argb);
  void clearBackground(uint32_t xid); // CWBackPixmap=None: no server bg fill
  void setParentRelative(uint32_t xid); // CWBackPixmap=ParentRelative

  // ParentRelative: walk parent chain, copy nearest ancestor's background_pixel.
  // Returns true if a parent with has_background_pixel was found and applied.
  bool resolveParentRelativeBackground(uint32_t xid);

  // Dynamic background resolution for ClearArea / fillWindowBackground.
  // If window has ParentRelative, walks parent chain at call time to find
  // the current background pixel. Returns true if a background was found.
  bool resolveBackgroundForClear(uint32_t xid, uint32_t& out_pixel) const;

  // border
  void setBorderWidth(uint32_t xid, uint16_t bw);
  void setBorderPixel(uint32_t xid, uint32_t pixel_argb);

  // Stacking order manipulation (ConfigureWindow CWStackMode)
  void raiseToTop(uint32_t xid);       // Above with no sibling
  void lowerToBottom(uint32_t xid);    // Below with no sibling
  void restackAbove(uint32_t xid, uint32_t sibling);  // place xid just above sibling
  void restackBelow(uint32_t xid, uint32_t sibling);  // place xid just below sibling

  // Reparent: change parent and position
  bool reparent(uint32_t xid, uint32_t newParent, int16_t x, int16_t y);

  // cursor
  void setCursor(uint32_t xid, uint32_t cursor_xid);
  uint32_t cursor(uint32_t xid) const; // optional
  
  // Bring-up behavior: keep each descendant’s x/y (relative to parent) unchanged,
  // but clamp w/h so the child fits within its *direct* parent’s bounds.
  // Marks dirty when a child’s size changes.
  void clampDescendantsToParent(uint32_t rootXid);

  // SHAPE extension
  void setShapeBounding(uint32_t xid, ShapeRegion region);
  void setShapeClip(uint32_t xid, ShapeRegion region);
  void setShapeInput(uint32_t xid, ShapeRegion region);
  ShapeRegion shapeBounding(uint32_t xid) const;
  ShapeRegion shapeInput(uint32_t xid) const;
  bool isShapedBounding(uint32_t xid) const;
  // Check if point is inside the window’s effective input shape.
  // Falls back to bounding shape if no input shape is set.
  bool isInShapeRegion(uint32_t xid, int16_t lx, int16_t ly) const;
  

private:
  struct WindowState {
    uint32_t xid = 0;
    uint32_t parent = 0;
    int16_t  x = 0;
    int16_t  y = 0;
    uint16_t w = 1;
    uint16_t h = 1;

    uint32_t cursor_xid = 0; // 0 means "inherit/default"

    uint32_t event_mask = 0;

    // X11 window background pixel (ARGB8888, alpha forced opaque).
    uint32_t background_pixel = 0;
    bool     has_background_pixel = false;
    bool     is_parent_relative = false;  // CWBackPixmap=ParentRelative

    // Window border (server-drawn around child windows)
    uint16_t border_width = 0;
    uint32_t border_pixel = 0xFF000000u; // ARGB, default black

    // SHAPE extension regions
    ShapeRegion shape_bounding;  // visual clipping
    ShapeRegion shape_clip;      // rendering clipping (subset of bounding)
    ShapeRegion shape_input;     // hit testing

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

  // Children in stacking order (bottom-to-top = creation order).
  // Key = parent XID, Value = ordered list of child XIDs.
  std::unordered_map<uint32_t, std::vector<uint32_t>> children_order_;

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
