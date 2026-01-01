#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*x11_window_created_cb)(
    uint32_t xwin_id,
    const char* title,
    int width,
    int height
);

typedef void (*x11_window_closed_cb)(
    uint32_t xwin_id
);

bool x11_start_server(int32_t display);
void x11_stop_server(void);

void x11_register_callbacks(
    x11_window_created_cb on_create,
    x11_window_closed_cb on_close
);

typedef void (*x11_present_frame_cb)(
    uint32_t xwin_id,
    const void* bgra,
    int width,
    int height,
    int bytes_per_row
);

void x11_register_frame_presenter(x11_present_frame_cb on_present);

  void x11_request_repaint(uint32_t xwin_id, int32_t width_px, int32_t height_px);  
#ifdef __cplusplus
}
#endif

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

void x11_post_key_event(uint32_t xwin_id,
                        bool is_down,
                        uint32_t keycode,
                        uint32_t modifiers,
                        const char* utf8_text);
