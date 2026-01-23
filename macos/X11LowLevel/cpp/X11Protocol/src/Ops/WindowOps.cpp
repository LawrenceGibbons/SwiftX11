//
//  WindowOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "WindowOps.hpp"

// Temporary stand-ins so this compiles before you introduce shared headers.
// Delete these once XProtoServerState / XProtoRequestContext exist for real.
struct XProtoServerState {};
struct XProtoRequestContext {};

void WindowOps::handleCreateWindow(uint8_t /*depth*/,
                                   const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - parse CreateWindow fixed fields (wid, parent, x/y, w/h, visual, valueMask)
  // - allocate window slot + backing store in xproto state
  // - record parent_xid, owner_fd, event_mask from value-list (CWEventMask)
  // - enqueue create request to shim (or push X11_EV_WINDOW_CREATE in unified event path)
}

void WindowOps::handleChangeWindowAttributes(const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - parse wid, valueMask, value-list
  // - apply CWEventMask (and later CWBackPixel/CWColormap/etc.)
}

void WindowOps::handleGetWindowAttributes(int /*clientFd*/, uint16_t /*seq*/,
                                          const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - reply with mapState, event masks, visual, colormap, etc.
  // - keep reply building centralized (ctx helpers)
}

void WindowOps::handleDestroyWindow(const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - remove window from xproto state, free backing store
  // - enqueue destroy request to shim and/or emit destroy event
}

void WindowOps::handleMapWindow(int /*clientFd*/, uint16_t /*seq*/,
                                const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - mark mapped=1 in xproto truth
  // - enqueue MAP to shim
  // - if window has pending dirty content and is presentable, enqueue damage
  // - if ExposureMask selected, send Expose event (queued, not written cross-thread)
}

void WindowOps::handleMapSubwindows(int /*clientFd*/, uint16_t /*seq*/,
                                    const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - walk subtree; mark children mapped
  // - enqueue MAP for each
  // - Expose per child if requested
}

void WindowOps::handleUnmapWindow(const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - mark mapped=0; defer damage until re-map
  // - enqueue UNMAP to shim
}

void WindowOps::handleConfigureWindow(int /*clientFd*/, uint16_t /*seq*/,
                                      const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - parse vmask+values; apply x/y/w/h
  // - if size changes: resize backing store (preserve overlap)
  // - enqueue CONFIGURE to shim
  // - if StructureNotifyMask: queue ConfigureNotify
  // - if ExposureMask and mapped: queue Expose
}

void WindowOps::handleGetGeometry(int /*clientFd*/, uint16_t /*seq*/,
                                  const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - reply with root, x/y, w/h, borderWidth, depth
}

void WindowOps::handleQueryTree(int /*clientFd*/, uint16_t /*seq*/,
                                const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - reply with root, parent, list of children
}
