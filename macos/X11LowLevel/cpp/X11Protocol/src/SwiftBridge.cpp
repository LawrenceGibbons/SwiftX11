//
//  SwiftBridge.cpp
//  X11LowLevel
//
//  C++ implementations of SwiftX11Bridge.h functions.
//  Replaces x11_shim.c — no C backend, no C request queue, no C runloop thread.
//

#include <cstdint>
#include <cstdio>

extern "C" {
#include "SwiftX11Bridge.h"
}

#include "Core/XProtoServer.hpp"
#include "Fonts/CoreTextFont.hpp"

// Forward: get the server instance (defined in XProtoServerBridge.cpp)
extern "C" x11::XProtoServer* x11_proto_bridge_get_server(void);

// ---- C++ bridge functions (defined in XProtoServerBridge.cpp) ----
extern "C" int  x11_proto_start_daemon(int display);
extern "C" int  x11_proto_start_daemon_ex(int display, int enable_tcp, int enable_unix,
                                          const char* tcp_bind_addr);
extern "C" void x11_proto_stop_daemon(void);

extern "C" void x11_proto_bridge_post_pointer_move2(uint32_t xid,
                                                     int32_t  win_x_u,  int32_t win_y_u,
                                                     int32_t root_x_u, int32_t root_y_u,
                                                     uint8_t deliver,
                                                     uint32_t buttons,
                                                     uint32_t modifiers);

extern "C" void x11_proto_bridge_post_pointer_button(uint32_t xid,
                                                      uint8_t is_press,
                                                      uint8_t button,
                                                      int32_t win_x_u, int32_t win_y_u,
                                                      int32_t root_x_u, int32_t root_y_u,
                                                      uint32_t buttons,
                                                      uint32_t modifiers);

extern "C" void x11_proto_bridge_post_scroll(uint32_t xid,
                                              uint8_t axis,
                                              int16_t ticks,
                                              int32_t win_x_u, int32_t win_y_u,
                                              uint32_t buttons,
                                              uint32_t modifiers);

extern "C" void x11_proto_bridge_post_key(uint32_t xid,
                                           uint8_t is_down,
                                           uint32_t keycode,
                                           uint32_t modifiers);

extern "C" void x11_proto_bridge_post_enter(uint32_t xid,
                                             int32_t win_x_u, int32_t win_y_u,
                                             uint32_t modifiers);

extern "C" void x11_proto_bridge_post_leave(uint32_t xid,
                                             int32_t win_x_u, int32_t win_y_u,
                                             uint32_t modifiers);

extern "C" void x11_proto_bridge_post_focus(uint32_t xid,
                                             uint8_t focused);

extern "C" void x11_proto_bridge_apply_rootless_resize(uint32_t xid,
                                                        int32_t w_px, int32_t h_px);

extern "C" void x11_proto_bridge_window_set_presentable_and_flush(uint32_t xid);

extern "C" void x11_proto_bridge_expose_children(uint32_t xid);

extern "C" int  x11_cpp_copy_host_surface_bgra(uint32_t xid,
                                                uint8_t* out_bytes,
                                                int32_t out_cap,
                                                int32_t* out_w,
                                                int32_t* out_h,
                                                int32_t* out_bpr);

// ===========================================================================
// Lifecycle
// ===========================================================================

extern "C" bool x11_start_server(int32_t display)
{
  // Start the C++ XProtoDaemon (poll-based listener + protocol engine).
  // No C backend, no C runloop thread.
  int ok = x11_proto_start_daemon((int)display);
  return ok ? true : false;
}

extern "C" bool x11_start_server_ex(int32_t display, bool enable_tcp, bool enable_unix,
                                     const char* tcp_bind_addr)
{
  int ok = x11_proto_start_daemon_ex((int)display,
                                     enable_tcp ? 1 : 0,
                                     enable_unix ? 1 : 0,
                                     tcp_bind_addr);
  return ok ? true : false;
}

extern "C" void x11_stop_server(void)
{
  x11_proto_stop_daemon();
}

// ===========================================================================
// Input injection — pass-throughs to C++ bridge (→ HostCommandQueue)
// ===========================================================================

extern "C" void x11_post_pointer_move2(uint32_t xid,
                                        int32_t  win_x_u, int32_t  win_y_u,
                                        int32_t root_x_u, int32_t root_y_u,
                                        uint8_t deliver,
                                        uint32_t buttons,
                                        uint32_t modifiers)
{
  if (xid == 0) return;
  x11_proto_bridge_post_pointer_move2(xid,
                                      win_x_u, win_y_u,
                                      root_x_u, root_y_u,
                                      deliver,
                                      buttons,
                                      modifiers);
}

extern "C" void x11_post_pointer_button(uint32_t xid,
                                         bool is_press,
                                         uint8_t button,
                                         int32_t x_u,
                                         int32_t y_u,
                                         int32_t root_x,
                                         int32_t root_y,
                                         uint32_t buttons,
                                         uint32_t modifiers)
{
  if (xid == 0) return;
  x11_proto_bridge_post_pointer_button(xid,
                                       is_press ? 1 : 0,
                                       button,
                                       x_u, y_u,
                                       root_x, root_y,
                                       buttons,
                                       modifiers);
}

extern "C" void x11_post_scroll_ticks(uint32_t xid,
                                       x11_scroll_axis_t axis,
                                       int16_t ticks,
                                       int32_t x_u,
                                       int32_t y_u,
                                       uint32_t buttons,
                                       uint32_t modifiers)
{
  if (xid == 0) return;
  x11_proto_bridge_post_scroll(xid,
                               (uint8_t)axis,
                               ticks,
                               x_u, y_u,
                               buttons,
                               modifiers);
}

extern "C" void x11_post_key_event(uint32_t xid, bool is_down,
                                    uint32_t keycode, uint32_t modifiers,
                                    const char* utf8_text)
{
  (void)utf8_text;
  x11_proto_bridge_post_key(xid,
                            is_down ? 1 : 0,
                            keycode,
                            modifiers);
}

extern "C" void x11_post_pointer_enter(uint32_t xid,
                                        int32_t x_u,
                                        int32_t y_u,
                                        uint32_t modifiers)
{
  if (xid == 0) return;
  x11_proto_bridge_post_enter(xid, x_u, y_u, modifiers);
}

extern "C" void x11_post_pointer_leave(uint32_t xid,
                                        int32_t x_u,
                                        int32_t y_u,
                                        uint32_t modifiers)
{
  if (xid == 0) return;
  x11_proto_bridge_post_leave(xid, x_u, y_u, modifiers);
}

// ===========================================================================
// Focus
// ===========================================================================

extern "C" void x11_post_focus_event(uint32_t xid, bool focused)
{
  if (xid == 0) return;
  x11_proto_bridge_post_focus(xid, focused ? 1 : 0);
}

// ===========================================================================
// Host/window-manager actions (Swift → server)
// ===========================================================================

extern "C" void x11_post_window_raise(uint32_t xid)
{
  x11_ui_push_raise(xid);
}

extern "C" void x11_post_window_map(uint32_t xid)
{
  if (xid == 0) return;
  x11_ui_push_map(xid);
}

extern "C" void x11_post_window_unmap(uint32_t xid)
{
  if (xid == 0) return;
  x11_ui_push_unmap(xid);
}

extern "C" void x11_post_window_destroy(uint32_t xid)
{
  if (xid == 0) return;
  x11_ui_push_destroy(xid);
}

extern "C" void x11_post_window_close(uint32_t xid)
{
  if (xid == 0) return;
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return;
  srv->hostCmds().push(x11::HostCmd{x11::HostCmdType::WindowClose, xid, 0, 0});
}

// ===========================================================================
// Host-driven resize (Cocoa changed window size)
// ===========================================================================

extern "C" void x11_post_window_resize(uint32_t xid, int32_t w_u, int32_t h_u)
{
  if (xid == 0) return;
  if (w_u < 1) w_u = 1;
  if (h_u < 1) h_u = 1;

  // Tell Swift/UI about the new size.
  x11_ui_push_resize(xid, w_u, h_u);

  // Queue rootless resize to the server thread (geometry update + ConfigureNotify).
  x11_proto_bridge_apply_rootless_resize(xid, w_u, h_u);
}

// ===========================================================================
// End-of-resize expose
// ===========================================================================

extern "C" void x11_post_expose_children(uint32_t xid)
{
  if (xid == 0) return;
  x11_proto_bridge_expose_children(xid);
}

// ===========================================================================
// Presentable
// ===========================================================================

extern "C" void x11_post_window_presentable(uint32_t xid)
{
  if (xid == 0) return;
  x11_proto_bridge_window_set_presentable_and_flush(xid);
}

// ===========================================================================
// BGRA copy (Swift → C++ surface registry)
// ===========================================================================

extern "C" int x11_server_copy_window_bgra(uint32_t xid,
                                            uint8_t* out_bytes,
                                            int32_t out_cap,
                                            int32_t* out_w,
                                            int32_t* out_h,
                                            int32_t* out_bpr)
{
  return x11_cpp_copy_host_surface_bgra(xid, out_bytes, out_cap, out_w, out_h, out_bpr);
}

// ===========================================================================
// Clipboard bridge (macOS NSPasteboard <-> X11 CLIPBOARD)
// ===========================================================================

static x11_clipboard_get_text_fn g_clip_get = nullptr;
static x11_clipboard_set_text_fn g_clip_set = nullptr;

extern "C" void x11_clipboard_register(x11_clipboard_get_text_fn get_fn,
                                         x11_clipboard_set_text_fn set_fn)
{
  g_clip_get = get_fn;
  g_clip_set = set_fn;
}

extern "C" uint32_t x11_clipboard_get_text(char* buf, uint32_t max_len)
{
  if (!g_clip_get || !buf || max_len == 0) return 0;
  return g_clip_get(buf, max_len);
}

extern "C" void x11_clipboard_set_text(const char* text, uint32_t len)
{
  if (!g_clip_set || !text || len == 0) return;
  g_clip_set(text, len);
}

static x11_clipboard_change_count_fn g_clip_cc = nullptr;

extern "C" void x11_clipboard_register_change_count(x11_clipboard_change_count_fn fn)
{
  g_clip_cc = fn;
}

extern "C" int64_t x11_clipboard_get_change_count(void)
{
  if (!g_clip_cc) return -1;
  return g_clip_cc();
}

// -------------------------------------------------------------------------------------
// Font antialiasing toggle
// -------------------------------------------------------------------------------------
extern "C" void x11_set_font_antialiased(int enabled)
{
  x11::font::setAntialiasedFonts(enabled != 0);
}

extern "C" int x11_get_font_antialiased(void)
{
  return x11::font::antialiasedFonts() ? 1 : 0;
}

// -------------------------------------------------------------------------------------
// SHAPE extension bridge
// -------------------------------------------------------------------------------------
extern "C" bool x11_shape_is_shaped(uint32_t xid) {
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) return false;
  return srv->windows().isShapedBounding(xid);
}

// -------------------------------------------------------------------------------------
// Window geometry query (for override-redirect positioning at map time)
// -------------------------------------------------------------------------------------
extern "C" bool x11_get_window_geometry(uint32_t xid, int32_t* out_x, int32_t* out_y,
                                        int32_t* out_w, int32_t* out_h,
                                        bool* out_override_redirect) {
  auto* srv = x11_proto_bridge_get_server();
  if (!srv) {
#ifndef NDEBUG
    fprintf(stderr, "[GEO_QUERY] xid=0x%08X FAIL: no server\n", (unsigned)xid);
#endif
    return false;
  }

  x11::WindowView vw;
  if (!srv->windows().snapshot(xid, vw)) {
#ifndef NDEBUG
    fprintf(stderr, "[GEO_QUERY] xid=0x%08X FAIL: not in WindowTable\n", (unsigned)xid);
#endif
    return false;
  }

  if (out_x) *out_x = vw.x;
  if (out_y) *out_y = vw.y;
  if (out_w) *out_w = vw.w;
  if (out_h) *out_h = vw.h;
  if (out_override_redirect) *out_override_redirect = vw.override_redirect;
#ifndef NDEBUG
  fprintf(stderr, "[GEO_QUERY] xid=0x%08X OK: pos=(%d,%d) size=%dx%d or=%d\n",
          (unsigned)xid, (int)vw.x, (int)vw.y, (int)vw.w, (int)vw.h,
          (int)vw.override_redirect);
#endif
  return true;
}

extern "C" int32_t x11_shape_get_rects(uint32_t xid, int16_t* out_xywh, int32_t max_rects) {
  auto* srv = x11_proto_bridge_get_server();
  if (!srv || !out_xywh || max_rects <= 0) return 0;

  x11::ShapeRegion region = srv->windows().shapeBounding(xid);
  if (!region.shaped) return 0;

  int32_t n = 0;
  for (size_t i = 0; i < region.rects.size() && n < max_rects; i++, n++) {
    out_xywh[n * 4 + 0] = region.rects[i].x;
    out_xywh[n * 4 + 1] = region.rects[i].y;
    out_xywh[n * 4 + 2] = (int16_t)region.rects[i].w;
    out_xywh[n * 4 + 3] = (int16_t)region.rects[i].h;
  }
  return n;
}
