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

#include <Core/WindowView.hpp>
#include <Core/InputState.hpp>

namespace x11 {

class XProtoTransport;
class ReplyWriter;
class WindowTable;
class PixmapTable;
class UICommandQueue;
class FontTable;
class CursorTable;
class GrabTable;
class DrawableSurfaceRegistry;
class XClient;

using WindowLookupFn = bool (*)(uint32_t xid, WindowView* out, void* user);

class XProtoContext {
public:
  XProtoContext() = default;

  // ---- Logging ----
  void tracef(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

  // ---- Per-client wiring ----
  // setClient wires transport + reply from the client into this context.
  // clearClient nulls the per-client pointers (server-wide state remains).
  void setClient(XClient* c);
  void clearClient();
  XClient* client() const { return client_; }
  bool hasClient() const { return client_ != nullptr; }

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

  // Mouse/input handling (server-wide — one pointer per display)
  void setInputState(InputState* is) { input_ = is; }
  InputState& input() { assert(input_); return *input_; }
  const InputState& input() const { assert(input_); return *input_; }


  // User Interface
  void setUI(UICommandQueue* q) { ui_ = q; }
  UICommandQueue& ui() { assert(ui_); return *ui_; }
  const UICommandQueue& ui() const { assert(ui_); return *ui_; }

  // Font Table
  void setFontTable(FontTable* ft) { font_table_ = ft; }
  FontTable& fonts() { assert(font_table_); return *font_table_; }
  const FontTable& fonts() const { assert(font_table_); return *font_table_; }

  // Cursor handling
  void setCursorTable(CursorTable* ct) { cursor_table_ = ct; }
  CursorTable& cursors() { assert(cursor_table_); return *cursor_table_; }
  const CursorTable& cursors() const { assert(cursor_table_); return *cursor_table_; }

  // Grab handling
  void setGrabTable(GrabTable* gt) { grab_table_ = gt; }
  GrabTable& grabs() { assert(grab_table_); return *grab_table_; }
  const GrabTable& grabs() const { assert(grab_table_); return *grab_table_; }

  // Drawable surfaces
  void setSurfaceRegistry(DrawableSurfaceRegistry* sr) { surface_registry_ = sr; }
  DrawableSurfaceRegistry& surfaces() { assert(surface_registry_); return *surface_registry_; }
  const DrawableSurfaceRegistry& surfaces() const { assert(surface_registry_); return *surface_registry_; }

private:
  // Per-client (set via setClient/clearClient)
  XClient* client_ = nullptr;
  XProtoTransport* transport_ = nullptr;
  ReplyWriter* reply_ = nullptr;

  // Server-wide (set once, persist across sessions)
  WindowTable* window_table_ = nullptr;

  WindowLookupFn lookup_ = nullptr;
  void* lookup_user_ = nullptr;

  // scratch storage to avoid allocations
  WindowView scratch_{};

  // Drawable Surface
  DrawableSurfaceRegistry* surface_registry_ = nullptr;

  // pixel maps
  PixmapTable* pixmap_table_ = nullptr;

  // Mouse handling (pointer to server-owned InputState)
  InputState* input_ = nullptr;

  // User Interface
  UICommandQueue* ui_ = nullptr;

  // Fonts
  FontTable* font_table_ = nullptr;

  // Cursor
  CursorTable* cursor_table_ = nullptr;

  // Grabs
  GrabTable* grab_table_ = nullptr;

};

} // namespace x11
