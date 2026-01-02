//
//  x11_shim.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/1/26.
//

#pragma once
#include <stdint.h>
#include <stdbool.h>

#include <X11LowLevel/x11_events.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Window lifecycle callbacks (Swift UI bridge)
typedef void (*x11_window_created_cb)(
    uint32_t xwin_id,
    const char* title,
    int width,
    int height
);

typedef void (*x11_window_closed_cb)(
    uint32_t xwin_id
);

// ---- Server lifecycle
bool x11_start_server(int32_t display);
void x11_stop_server(void);

void x11_register_callbacks(
    x11_window_created_cb on_create,
    x11_window_closed_cb on_close
);

// ---- Frame presentation (C -> Swift)
typedef void (*x11_present_frame_cb)(
    uint32_t xwin_id,
    const void* bgra,
    int width,
    int height,
    int bytes_per_row
);

void x11_register_frame_presenter(x11_present_frame_cb on_present);

void x11_request_repaint(uint32_t xwin_id, int32_t width_px, int32_t height_px);  

typedef enum {
    X11_PTR_MOVE = 0,
    X11_PTR_DOWN = 1,
    X11_PTR_UP   = 2,
    X11_SCROLL   = 3
} x11_ptr_event_type;


void x11_post_pointer_event(uint32_t xwin_id,
                            x11_ptr_event_type type,
                            int32_t x_px,
                            int32_t y_px,
                            uint32_t buttons,
                            uint32_t modifiers);

// ---- New “typed” input APIs (Option A)
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

void x11_post_focus_event(uint32_t xid, bool focused);
  
void x11_post_pointer_enter(uint32_t xid,
                            int32_t x_px,
                            int32_t y_px,
                            uint32_t modifiers);

void x11_post_pointer_leave(uint32_t xid,
                            int32_t x_px,
                            int32_t y_px,
                            uint32_t modifiers);
  
bool x11_debug_pop_event(x11_event_t* out_ev);
  
#ifdef __cplusplus
}
#endif
