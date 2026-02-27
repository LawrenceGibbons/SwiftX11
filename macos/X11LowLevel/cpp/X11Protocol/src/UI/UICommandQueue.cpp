//
//  UICommandQueue.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/6/26.
//

//
//  UICommandQueue.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/6/26.
//

#include "UI/UICommandQueue.hpp"

#include <cstdint>

extern "C" {
#include "x11_requests.h"
#include "SwiftX11Bridge.h"
}

namespace x11 {

static inline int32_t clamp_dim(int32_t v) { return (v < 1) ? 1 : v; }
static inline const char* safe_cstr(const char* s) { return s ? s : ""; }

bool UICommandQueue::push(const UICommand& c)
{
  switch (c.type) {
    case UICommand::Type::Create: {
      const int32_t w = clamp_dim(c.w_px);
      const int32_t h = clamp_dim(c.h_px);
      return x11_requests_push_create(c.xid, c.parent, safe_cstr(c.title_utf8), w, h) != 0;
    }

    case UICommand::Type::Destroy:
      return x11_requests_push_destroy(c.xid) != 0;

    case UICommand::Type::Map:
      return x11_requests_push_map(c.xid) != 0;

    case UICommand::Type::Unmap:
      return x11_requests_push_unmap(c.xid) != 0;

    case UICommand::Type::Configure: {
      const int32_t w = clamp_dim(c.w_px);
      const int32_t h = clamp_dim(c.h_px);
      return x11_requests_push_configure(c.xid, w, h) != 0;
    }

    case UICommand::Type::SetTitle:
      return x11_requests_push_set_title(c.xid, safe_cstr(c.title_utf8)) != 0;

    case UICommand::Type::Presentable:
      return x11_requests_push_window_presentable(c.xid) != 0;

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
  x11_requests_drain_on_server_thread();
}

} // namespace x11
