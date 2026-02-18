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
} x11_ui_cmd_type_t;

typedef struct {
  x11_ui_cmd_type_t type;
  uint32_t          xid;
  uint32_t          parent_xid;
  int32_t           x_u, y_u, w_u, h_u;  // used by DAMAGE/RESIZE/CREATE
  uint8_t           title_len;
  char              title_utf8[32];
} x11_ui_cmd_t;

// -------------------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------------------
bool x11_start_server(int32_t display);
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
void x11_ui_push_create(uint32_t xid, uint32_t parent_xid, int32_t w_px, int32_t h_px);
void x11_ui_push_destroy(uint32_t xid);
void x11_ui_push_damage(uint32_t xid, int32_t x_px, int32_t y_px, int32_t w_px, int32_t h_px);


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

// -------------------------------------------------------------------------------------
// Host-driven resize (Cocoa changed size)
// -------------------------------------------------------------------------------------
void x11_post_window_resize(uint32_t xid, int32_t w_u, int32_t h_u);
