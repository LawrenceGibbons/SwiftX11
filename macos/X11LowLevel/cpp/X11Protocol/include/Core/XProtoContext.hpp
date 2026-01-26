//
//  XProtoContext.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdarg>

#include "WindowView.hpp"

namespace x11 {

class XProtoTransport;
class ReplyWriter;
class WindowTable;
  
// Option A: XProtoContext provides what EventOps needs via callbacks.
// (C world owns the truth; C++ asks for a snapshot.)
using WindowLookupFn = bool (*)(uint32_t xid, WindowView* out, void* user);

class XProtoContext {
public:
  XProtoContext() = default;

  // ---- Logging ----
  void tracef(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  // ---- Transport wiring ----
  void setTransport(XProtoTransport* t) { transport_ = t; }
  XProtoTransport& transport(); // asserts non-null

  // reply sending
  ReplyWriter& reply() { return *reply_; }
  void setReplyWriter(ReplyWriter* r) { reply_ = r; }

  // ---- Window snapshot lookup wiring ----
  void setWindowLookup(WindowLookupFn fn, void* user) {
    lookup_ = fn;
    lookup_user_ = user;
  }

  // Returns nullptr if not found.
  // Note: returned pointer is only valid until the next call (uses scratch_).
  const WindowView* window(uint32_t xid);

  void setWindowTable(WindowTable* wt) { window_table_ = wt; } 
  WindowTable& windows() { return *window_table_; } // assert non-null in impl
  
private:
  XProtoTransport* transport_ = nullptr;
  ReplyWriter* reply_ = nullptr;
  
  WindowTable* window_table_ = nullptr; 

  WindowLookupFn lookup_ = nullptr;
  void* lookup_user_ = nullptr;

  // scratch storage to avoid allocations
  WindowView scratch_{};
};

} // namespace x11
