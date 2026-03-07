//
//  UICommandQueue.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/6/26.
//

#include "UI/UICommandQueue.hpp"

#include <cstdint>

extern "C" {
#include "SwiftX11Bridge.h"
}

// Pushes to HostCommandQueue (already defined in XProtoServerBridge.cpp)
extern "C" void x11_proto_bridge_window_set_presentable_and_flush(uint32_t xid);

namespace x11 {

static inline int32_t clamp_dim(int32_t v) { return (v < 1) ? 1 : v; }
static inline const char* safe_cstr(const char* s) { return s ? s : ""; }

bool UICommandQueue::push(const UICommand& c)
{
  switch (c.type) {
    case UICommand::Type::Create: {
      const int32_t w = clamp_dim(c.w_px);
      const int32_t h = clamp_dim(c.h_px);
      x11_ui_push_create(c.xid, c.parent, w, h, c.flags);
      return true;
    }

    case UICommand::Type::Destroy:
      x11_ui_push_destroy(c.xid);
      return true;

    case UICommand::Type::Map:
      x11_ui_push_map(c.xid);
      return true;

    case UICommand::Type::Unmap:
      x11_ui_push_unmap(c.xid);
      return true;

    case UICommand::Type::Configure: {
      const int32_t w = clamp_dim(c.w_px);
      const int32_t h = clamp_dim(c.h_px);
      x11_ui_push_resize(c.xid, w, h);
      return true;
    }

    case UICommand::Type::SetTitle:
      x11_ui_push_title(c.xid, safe_cstr(c.title_utf8));
      return true;

    case UICommand::Type::Presentable:
      // Pushes to HostCommandQueue for xproto-thread processing.
      x11_proto_bridge_window_set_presentable_and_flush(c.xid);
      return true;

    case UICommand::Type::SetCursor:
      // Interpret c.xid as host_xid for rootless cursor updates.
      x11_ui_push_set_cursor(c.xid, c.cursor_xid, c.shape);
      return true;

    default:
      return false;
  }
}

void UICommandQueue::drainOnServerThread()
{
  // No-op: C request queue eliminated.
  // All commands are now pushed directly to the UI queue or HostCommandQueue.
}

} // namespace x11
