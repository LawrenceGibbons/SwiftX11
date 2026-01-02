//
//  x11_events.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/1/26.
//

#ifndef x11_events_h
#define x11_events_h

#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Modifiers (your existing mapping is fine)
enum {
    X11_MOD_SHIFT   = 1u << 0,
    X11_MOD_CTRL    = 1u << 1,
    X11_MOD_ALT     = 1u << 2,
    X11_MOD_CMD     = 1u << 3,
};

// ---- Pointer button bits: button N sets bit (N-1)
static inline uint32_t x11_button_bit(uint8_t button_number /* 1..31 */) {
    return (button_number >= 1 && button_number <= 31) ? (1u << (button_number - 1)) : 0u;
}

// ---- Event types
  typedef enum {
    X11_EV_NONE = 0,
    
    X11_EV_POINTER_MOTION = 1,
    X11_EV_POINTER_BUTTON = 2,
    X11_EV_SCROLL         = 3,
    
    X11_EV_KEY            = 4,
    X11_EV_MODIFIERS      = 5,
    X11_EV_FOCUS          = 6,
    X11_EV_WINDOW_CREATE  = 7,
    X11_EV_WINDOW_DESTROY = 8,
    X11_EV_POINTER_ENTER  = 9,
    X11_EV_POINTER_LEAVE  = 10,
    X11_EV_WINDOW_RAISE   = 11,
  } x11_event_type_t;

// ---- Scroll “wheels” (X11-ish)
typedef enum {
    X11_SCROLL_VERT = 0,
    X11_SCROLL_HORZ = 1,
} x11_scroll_axis_t;

// ---- Event payloads
typedef struct {
    int32_t  x_px;
    int32_t  y_px;
    uint32_t buttons;     // current button mask
    uint32_t modifiers;   // X11_MOD_*
} x11_pointer_motion_t;

typedef struct {
    int32_t  x_px;
    int32_t  y_px;
    uint8_t  button;      // 1..31 (1=left, 2=middle, 3=right, 4/5/6/7 wheel if you choose)
    uint8_t  is_press;    // 1=press, 0=release
    uint16_t _pad;
    uint32_t buttons;     // current button mask AFTER state change (recommended)
    uint32_t modifiers;   // X11_MOD_*
} x11_pointer_button_t;

typedef struct {
    int32_t  x_px;
    int32_t  y_px;
    int16_t  ticks;       // + = up/right, - = down/left (one tick per threshold)
    uint8_t  axis;        // x11_scroll_axis_t
    uint8_t  _pad0;
    uint32_t buttons;     // current button mask (usually unchanged)
    uint32_t modifiers;   // X11_MOD_*
} x11_scroll_t;

#define X11_TEXT_MAX 32

typedef struct {
    uint32_t keycode;     // platform keycode (macOS keyCode for now)
    uint8_t  is_press;    // 1=down, 0=up
    uint8_t  text_len;    // 0..X11_TEXT_MAX
    uint16_t _pad;
    uint32_t modifiers;   // X11_MOD_*
    char     text_utf8[X11_TEXT_MAX]; // only meaningful on key press
} x11_key_t;

typedef struct {
    uint32_t modifiers;   // full current modifier state
} x11_modifiers_t;

typedef struct {
    uint8_t  focused;     // 1=focused, 0=unfocused
    uint8_t  _pad[3];
} x11_focus_t;

typedef struct {
    int32_t width_px;
    int32_t height_px;
} x11_window_create_t;
  
typedef struct {
    uint8_t _pad; // nothing for now, reserved
} x11_window_destroy_t;
  
typedef struct {
    int32_t x_px;
    int32_t y_px;
    uint32_t modifiers;
} x11_pointer_crossing_t;
  
typedef struct {
    uint8_t reserved;
} x11_window_raise_t;
  
// ---- Unified event
typedef struct {
    uint64_t         timestamp_ns; // monotonic, optional but nice
    uint32_t         xid;          // target window
    x11_event_type_t type;         // x11_event_type_t
    uint16_t         size;         // sizeof(payload) used (defensive)
    union {
        x11_pointer_motion_t   motion;
        x11_pointer_button_t   button;
        x11_scroll_t           scroll;
        x11_key_t              key;
        x11_modifiers_t        mods;
        x11_focus_t            focus;
        x11_window_create_t    win_create;
        x11_window_destroy_t   win_destroy;
        x11_pointer_crossing_t crossing;
        x11_window_raise_t     raise;
    } u;
} x11_event_t;

// ---- Queue API
void   x11_events_init(void);
void   x11_events_shutdown(void);

// returns false if queue full (event dropped)
bool   x11_events_push(const x11_event_t* ev);

// returns false if queue empty
bool   x11_events_pop(x11_event_t* out_ev);

// optional helpers
uint32_t x11_events_count(void);
void     x11_events_clear(void);



#ifdef __cplusplus
} // extern "C"
#endif

#endif /* x11_events_h */
