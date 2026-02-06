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
// Modifier efinitions
// -------------------------------------------------------------------------------------
enum {
  X11_MOD_SHIFT   = 1u << 0,
  X11_MOD_CTRL    = 1u << 1,
  X11_MOD_ALT     = 1u << 2,
  X11_MOD_CMD     = 1u << 3,
};

typedef enum {
  X11_SCROLL_VERT = 0,
  X11_SCROLL_HORZ = 1,
} x11_scroll_axis_t;

// -------------------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------------------
bool x11_start_server(int32_t display);

void x11_stop_server(void);


// -------------------------------------------------------------------------------------
// Callback registration (eventually shift to queue polling
// -------------------------------------------------------------------------------------
//void x11_register_callbacks(
//    x11_window_created_cb on_create,
//    x11_window_closed_cb on_close
//);


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
                             int32_t x_px,
                             int32_t y_px,
                             uint32_t buttons,      // current button mask AFTER state update
                             uint32_t modifiers);

void x11_post_scroll_ticks(uint32_t xid,
                           x11_scroll_axis_t axis,  // X11_SCROLL_VERT / X11_SCROLL_HORZ
                           int16_t ticks,           // +up/right, -down/left
                           int32_t x_px,
                           int32_t y_px,
                           uint32_t buttons,
                           uint32_t modifiers);

void x11_post_key_event(uint32_t xwin_id,
                        bool is_down,
                        uint32_t keycode,
                        uint32_t modifiers,
                        const char* utf8_text);

void x11_post_pointer_enter(uint32_t xid,
                            int32_t x_px,
                            int32_t y_px,
                            uint32_t modifiers);

void x11_post_pointer_leave(uint32_t xid,
                            int32_t x_px,
                            int32_t y_px,
                            uint32_t modifiers);
  


// -------------------------------------------------------------------------------------
// Presentable
// -------------------------------------------------------------------------------------
void x11_post_window_presentable(uint32_t xid);


