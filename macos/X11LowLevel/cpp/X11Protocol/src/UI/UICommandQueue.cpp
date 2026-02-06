//
//  UICommandQueue.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/6/26.
//

#include "UICommandQueue.hpp"

extern "C" {
  #include "x11_requests.h"
}

namespace x11 {

bool UICommandQueue::pushCreate(uint32_t xid, uint32_t parent, const char* title_utf8, int32_t w_px, int32_t h_px) {
  return x11_requests_push_create(xid, parent, title_utf8, w_px, h_px) != 0;
}
bool UICommandQueue::pushDestroy(uint32_t xid) { return x11_requests_push_destroy(xid) != 0; }
bool UICommandQueue::pushMap(uint32_t xid) { return x11_requests_push_map(xid) != 0; }
bool UICommandQueue::pushUnmap(uint32_t xid) { return x11_requests_push_unmap(xid) != 0; }
bool UICommandQueue::pushConfigure(uint32_t xid, int32_t w_px, int32_t h_px) { return x11_requests_push_configure(xid, w_px, h_px) != 0; }
bool UICommandQueue::pushSetTitle(uint32_t xid, const char* title_utf8) { return x11_requests_push_set_title(xid, title_utf8) != 0; }
bool UICommandQueue::pushDamage(uint32_t xid) { return x11_requests_push_damage(xid) != 0; }
bool UICommandQueue::pushPresentable(uint32_t xid) { return x11_requests_push_window_presentable(xid) != 0; }

bool UICommandQueue::pushRootlessResize(uint32_t xid, int32_t w_px, int32_t h_px) {
  return x11_requests_push_rootless_resize(xid, w_px, h_px) != 0;
}

void UICommandQueue::drainOnServerThread() {
  x11_requests_drain_on_server_thread();
}

} // namespace x11
