//
//  UICommandQueue.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/6/26.
//

#pragma once
#include <cstdint>

namespace x11 {

class UICommandQueue {
public:
  // Server->UI commands (these currently map 1:1 to x11_requests_push_*)
  bool pushCreate(uint32_t xid, uint32_t parent, const char* title_utf8, int32_t w_px, int32_t h_px);
  bool pushDestroy(uint32_t xid);
  bool pushMap(uint32_t xid);
  bool pushUnmap(uint32_t xid);
  bool pushConfigure(uint32_t xid, int32_t w_px, int32_t h_px);
  bool pushSetTitle(uint32_t xid, const char* title_utf8);
  bool pushDamage(uint32_t xid);
  bool pushPresentable(uint32_t xid);

  // Server->UI cursor update for a *host* NSWindow (rootless).
  // host_xid: top-level host window (the Cocoa window backing store)
  // cursor_xid: X cursor resource ID (0 => default/inherit)
  bool pushSetCursor(uint32_t host_xid, uint32_t cursor_xid, int32_t shape);
  
  // Host->server “rootless resize” (Swift/UI)
  bool pushRootlessResize(uint32_t xid, int32_t w_px, int32_t h_px);

  // Drain on server thread (Swift side calls today)
  void drainOnServerThread();
};

} // namespace x11
