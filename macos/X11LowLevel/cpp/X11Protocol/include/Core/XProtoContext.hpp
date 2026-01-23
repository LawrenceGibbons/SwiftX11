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

namespace x11 {

class XProtoTransport;

// A “view” of an X11 window needed for event emission.
// This is intentionally NOT x11_win_t (keeps C globals isolated).
struct WindowView {
  uint32_t xid = 0;
  int16_t  x = 0;
  int16_t  y = 0;
  uint16_t w = 0;
  uint16_t h = 0;

  uint32_t event_mask = 0; // X11 SelectInput mask bits
  bool mapped = false;
  int  owner_fd = -1;      // client socket for this window
};

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

  // ---- Window snapshot lookup wiring ----
  void setWindowLookup(WindowLookupFn fn, void* user) {
    lookup_ = fn;
    lookup_user_ = user;
  }

  // Returns nullptr if not found.
  // Note: returned pointer is only valid until the next call (uses scratch_).
  const WindowView* window(uint32_t xid);

private:
  XProtoTransport* transport_ = nullptr;

  WindowLookupFn lookup_ = nullptr;
  void* lookup_user_ = nullptr;

  // scratch storage to avoid allocations
  WindowView scratch_{};
};

} // namespace x11
