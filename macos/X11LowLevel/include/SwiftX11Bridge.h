//
//  SwiftX11Bridge.h
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/5/26.
//

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// -------------------------------------------------------------------------------------
// Definitions
// -------------------------------------------------------------------------------------

typedef enum {
  X11_SCROLL_VERT = 0,
  X11_SCROLL_HORZ = 1,
} x11_scroll_axis_t;

typedef enum {
  X11_UI_NONE = 0,
  X11_UI_TITLE,
  X11_UI_RAISE,
  X11_UI_MAP,
  X11_UI_UNMAP,
  X11_UI_RESIZE,
  X11_UI_CREATE,
  X11_UI_DESTROY,
  X11_UI_DAMAGE,
  X11_UI_SET_CURSOR,
  X11_UI_WARP_POINTER,
  X11_UI_SHAPE_CHANGED,
  X11_UI_MOVE,
  X11_UI_SIZE_HINTS,      // WM_NORMAL_HINTS: min/max/increment via size_hints
  X11_UI_WINDOW_TYPE,     // _NET_WM_WINDOW_TYPE: type_atom in flags
  X11_UI_INITIAL_STATE,   // WM_HINTS initial_state: state in flags (1=Normal, 3=Iconic)
} x11_ui_cmd_type_t;

// SwiftX11Bridge.h

typedef struct {
  uint32_t host_xid;      // which rootless host window (NSWindow) to apply cursor to
  uint32_t cursor_xid;    // X11 cursor resource id (0 => default/inherit)
  int32_t  shape;         // your CursorShape enum value (stable int32)
  int32_t  reserved;      // future (theme, flags, etc.)
} x11_ui_cursor_t;

// Window flags (bitmask for x11_ui_cmd_t.flags)
#define X11_UI_FLAG_OVERRIDE_REDIRECT  (1u << 0)

typedef struct {
  x11_ui_cmd_type_t type;
  uint32_t          xid;
  uint32_t          parent_xid;
  int32_t           x_u, y_u, w_u, h_u;  // used by DAMAGE/RESIZE/CREATE
  uint32_t          flags;                // X11_UI_FLAG_* bits (used by CREATE)
  uint8_t           title_len;
  char              title_utf8[32];

  // explicit cursor payload (only meaningful for X11_UI_SET_CURSOR)
  x11_ui_cursor_t   cursor;

  // WM_NORMAL_HINTS size hints (only meaningful for X11_UI_SIZE_HINTS)
  int32_t min_w, min_h, max_w, max_h, inc_w, inc_h;

} x11_ui_cmd_t;

// -------------------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------------------
bool x11_start_server(int32_t display);
bool x11_start_server_ex(int32_t display, bool enable_tcp, bool enable_unix,
                         const char* tcp_bind_addr);
void x11_stop_server(void);


// -------------------------------------------------------------------------------------
// Event queueing
// -------------------------------------------------------------------------------------
// Pop one command; returns false if queue empty
bool x11_ui_pop_command(x11_ui_cmd_t* out_cmd);

// -------------------------------------------------------------------------------------
// Push APIs used by your C/C++ server side
// -------------------------------------------------------------------------------------
void x11_ui_push_title(uint32_t xid, const char* title_utf8);
void x11_ui_push_raise(uint32_t xid);
void x11_ui_push_map(uint32_t xid);
void x11_ui_push_unmap(uint32_t xid);
void x11_ui_push_resize(uint32_t xid, int32_t w_px, int32_t h_px);
void x11_ui_push_create(uint32_t xid, uint32_t parent_xid,
                        int32_t x_px, int32_t y_px, int32_t w_px, int32_t h_px,
                        uint32_t flags);
void x11_ui_push_destroy(uint32_t xid);
void x11_ui_push_damage(uint32_t xid, int32_t x_px, int32_t y_px, int32_t w_px, int32_t h_px);
void x11_ui_push_set_cursor(uint32_t host_xid, uint32_t cursor_xid, int32_t shape);
// WarpPointer: host_xid=0 means root-relative (screen coords); otherwise host-window-local.
void x11_ui_push_warp_pointer(uint32_t host_xid, int32_t x, int32_t y);
void x11_ui_push_shape_changed(uint32_t host_xid);
void x11_ui_push_move(uint32_t xid, int32_t x_px, int32_t y_px);
// ICCCM / EWMH: WM_NORMAL_HINTS, WM_HINTS, _NET_WM_WINDOW_TYPE
void x11_ui_push_size_hints(uint32_t xid, int32_t min_w, int32_t min_h,
                            int32_t max_w, int32_t max_h,
                            int32_t inc_w, int32_t inc_h);
void x11_ui_push_window_type(uint32_t xid, uint32_t type_atom);
void x11_ui_push_initial_state(uint32_t xid, uint32_t state);

// SHAPE extension — query shape data from C++ (called by Swift at present time)
bool x11_shape_is_shaped(uint32_t xid);
// Returns number of rects written. Each rect = 4 int16_t (x,y,w,h).
int32_t x11_shape_get_rects(uint32_t xid, int16_t* out_xywh, int32_t max_rects);

// Query X11 window geometry (called by Swift at map time for override-redirect positioning)
// Returns true if window found. Coordinates are in X11 root space (y-down).
bool x11_get_window_geometry(uint32_t xid, int32_t* out_x, int32_t* out_y,
                             int32_t* out_w, int32_t* out_h,
                             bool* out_override_redirect);

// Update X11 window position in WindowTable (called by Swift when server-side
// adjusts OR popup position to match cursor screen after monitor hot-plug).
void x11_set_window_position(uint32_t xid, int32_t x, int32_t y);

// -------------------------------------------------------------------------------------
// Input injection
// -------------------------------------------------------------------------------------
void x11_post_pointer_move2(uint32_t xid,
                            int32_t win_x, int32_t win_y,
                            int32_t root_x, int32_t root_y,
                            uint8_t deliver,
                            uint32_t buttons,
                            uint32_t modifiers);
  
void x11_post_pointer_button(uint32_t xid,
                             bool is_press,
                             uint8_t button,        // 1..31 (1=left, 2=middle, 3=right, 4..7 wheel if desired)
                             int32_t x_u,
                             int32_t y_u,
                             int32_t root_x,
                             int32_t root_y,
                             uint32_t buttons,      // current button mask AFTER state update
                             uint32_t modifiers);

void x11_post_scroll_ticks(uint32_t xid,
                           x11_scroll_axis_t axis,  // X11_SCROLL_VERT / X11_SCROLL_HORZ
                           int16_t ticks,           // +up/right, -down/left
                           int32_t x_u,
                           int32_t y_u,
                           uint32_t buttons,
                           uint32_t modifiers);

void x11_post_key_event(uint32_t xwin_id,
                        bool is_down,
                        uint32_t keycode,
                        uint32_t modifiers,
                        const char* utf8_text);

void x11_post_pointer_enter(uint32_t xid,
                            int32_t x_u,
                            int32_t y_u,
                            uint32_t modifiers);

void x11_post_pointer_leave(uint32_t xid,
                            int32_t x_u,
                            int32_t y_u,
                            uint32_t modifiers);
  


// -------------------------------------------------------------------------------------
// Presentable
// -------------------------------------------------------------------------------------
void x11_post_window_presentable(uint32_t xid);

// -------------------------------------------------------------------------------------
// make the one authoritative bgra copy visible to Swift
// -------------------------------------------------------------------------------------
int x11_server_copy_window_bgra(uint32_t xid,
                                uint8_t* out_bytes,
                                int32_t out_cap,
                                int32_t* out_w,
                                int32_t* out_h,
                                int32_t* out_bpr);

// -------------------------------------------------------------------------------------
// Host/window-manager actions (Swift -> server)
// -------------------------------------------------------------------------------------
void x11_post_focus_event(uint32_t xid, bool focused);
void x11_post_window_raise(uint32_t xid);
void x11_post_window_map(uint32_t xid);
void x11_post_window_unmap(uint32_t xid);
void x11_post_window_destroy(uint32_t xid);

// Window close request (red button / Cmd+W).
// Sends WM_DELETE_WINDOW ClientMessage if client supports it,
// otherwise forcefully disconnects the client.
void x11_post_window_close(uint32_t xid);

// -------------------------------------------------------------------------------------
// Host-driven resize (Cocoa changed size)
// -------------------------------------------------------------------------------------
void x11_post_window_resize(uint32_t xid, int32_t w_u, int32_t h_u);

// Force Expose to all mapped children (no BG fill).
// Call after live resize ends to recover child content lost during surface reallocation.
void x11_post_expose_children(uint32_t xid);

void x11_surface_update(uint32_t host_xid,
                        void* ptr,
                        uint32_t bytes_per_row,
                        uint16_t w,
                        uint16_t h,
                        uint32_t generation);

void x11_surface_clear(uint32_t host_xid);

// -------------------------------------------------------------------------------------
// Shared damage accumulator  (C++ writes, Swift reads — no queue latency)
// -------------------------------------------------------------------------------------
// Union a damage rect for `host_xid`.  Thread-safe; called from the server thread.
void x11_shared_damage_union(uint32_t host_xid,
                             int32_t x, int32_t y,
                             int32_t w, int32_t h);

// Consume (read + clear) the accumulated damage rect for `host_xid`.
// Returns true if a non-empty rect was accumulated; false otherwise.
// Thread-safe; called from the Swift main thread at present time.
bool x11_shared_damage_consume(uint32_t host_xid,
                               int32_t* out_x, int32_t* out_y,
                               int32_t* out_w, int32_t* out_h);

// Remove any accumulated damage for a destroyed host window.
void x11_shared_damage_clear(uint32_t host_xid);

// -------------------------------------------------------------------------------------
// Clipboard bridge (macOS NSPasteboard <-> X11 CLIPBOARD selection)
// -------------------------------------------------------------------------------------
// Getter: copies current macOS pasteboard text into buf (up to max_len bytes).
//         Returns number of bytes written (0 if clipboard empty or no text).
typedef uint32_t (*x11_clipboard_get_text_fn)(char* buf, uint32_t max_len);

// Setter: pushes text to macOS pasteboard.
typedef void (*x11_clipboard_set_text_fn)(const char* text, uint32_t len);

// Register clipboard callbacks. Called from Swift at server startup.
void x11_clipboard_register(x11_clipboard_get_text_fn get_fn,
                             x11_clipboard_set_text_fn set_fn);

// Internal: read macOS clipboard text. Returns byte count (0 if unavailable).
uint32_t x11_clipboard_get_text(char* buf, uint32_t max_len);

// Internal: write text to macOS clipboard.
void x11_clipboard_set_text(const char* text, uint32_t len);

// Change-count: returns NSPasteboard.general.changeCount so C++ can detect
// when the macOS clipboard has been updated externally (by another macOS app).
typedef int64_t (*x11_clipboard_change_count_fn)(void);
void x11_clipboard_register_change_count(x11_clipboard_change_count_fn fn);
int64_t x11_clipboard_get_change_count(void);

// -------------------------------------------------------------------------------------
// Font antialiasing toggle
// -------------------------------------------------------------------------------------
// Set/get whether server-side text rendering uses antialiased (8-bit alpha) glyphs.
// When enabled, CoreText-rasterized fonts are rendered with per-pixel alpha blending.
// When disabled, glyphs use 1-bit bitmaps (crisp/bitmap style).
void x11_set_font_antialiased(int enabled);
int  x11_get_font_antialiased(void);

// -------------------------------------------------------------------------------------
// Version
// -------------------------------------------------------------------------------------
const char* swiftx11_version(void);

#ifdef __cplusplus
}
#endif

