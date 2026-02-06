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
#include <cassert>

#include <WindowView.hpp>
#include <InputState.hpp>

namespace x11 {

class XProtoTransport;
class ReplyWriter;
class WindowTable;
class PixmapTable;
class UICommandQueue;
  
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
  
  // Read-only access (queries, snapshots, QueryOps, etc.)
  const WindowTable& windows() const;
  
  // Mutating access (CreateWindow, ConfigureWindow, Map/Unmap, etc.)
  WindowTable& windows();
  
  void setPixmapTable(PixmapTable* pt) { pixmap_table_ = pt; }
  
  PixmapTable& pixmaps() { assert(pixmap_table_); return *pixmap_table_; }
  
  const PixmapTable& pixmaps() const { assert(pixmap_table_); return *pixmap_table_; }
  
  // Mouse handling
  InputState& input() { return input_; }
  const InputState& input() const { return input_; }

  
  // User Interface -- this should eventually migrate to XProtoServer
  void setUI(UICommandQueue* q) { ui_ = q; }
  UICommandQueue& ui() { assert(ui_); return *ui_; }
  const UICommandQueue& ui() const { assert(ui_); return *ui_; }
  
private:
  XProtoTransport* transport_ = nullptr;
  ReplyWriter* reply_ = nullptr;
  
  WindowTable* window_table_ = nullptr; 

  WindowLookupFn lookup_ = nullptr;
  void* lookup_user_ = nullptr;

  // scratch storage to avoid allocations
  WindowView scratch_{};
  
  // pixel maps
  PixmapTable* pixmap_table_ = nullptr;
  
  // Mouse handling
  InputState input_;
  
  // User Interface
  UICommandQueue* ui_ = nullptr;
};

} // namespace x11
