//
//  x11_shim.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/1/26.
//

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
void x11_post_window_raise(uint32_t xid);
void x11_post_window_destroy(uint32_t xid);
void x11_post_window_destroy_async(uint32_t xid);
  

// ---- Debug helpers (window table validation)
void x11_debug_dump_window_table(void);
int  x11_debug_get_window_alive(uint32_t xid);
int  x11_debug_get_window_size(uint32_t xid, int32_t* out_w_px, int32_t* out_h_px);
// Enable/disable a repaint storm for stress-testing; xid=0 selects a default window.
void x11_debug_set_repaint_storm(int enabled, uint32_t xid);
void x11_debug_destroy_during_next_repaint(int enabled, uint32_t xid);

// ---- Debug helpers (routing snapshot)
// Prints a one-line snapshot of routing state/counters to stderr.
void x11_debug_dump_routing_snapshot(const char* reason);
// Resets routing-related debug counters.
void x11_debug_reset_routing_counters(void);
  
  
  // ---- Debug snapshot (for UI inspector)
  #ifndef X11_DEBUG_MAX_WINDOWS
  #define X11_DEBUG_MAX_WINDOWS 64
  #endif

typedef struct {
    uint32_t xid;
    int32_t  w_px;
    int32_t  h_px;
    uint8_t  alive;
    uint8_t  damaged;
    uint16_t _pad;
} x11_debug_window_row_t;

typedef struct {
    // Event queue stats
    uint32_t q_count;
    uint32_t _pad0;
    uint64_t q_motion_overwrites;
    uint64_t q_push_drops;
    uint64_t destroy_waits;
  
    // Routing state
    uint32_t pointer_xid;
    uint32_t focus_xid;
    uint32_t drag_xid;
    uint32_t buttons;

    // Window table snapshot
    uint32_t window_count;
    uint32_t _pad1;
    x11_debug_window_row_t windows[X11_DEBUG_MAX_WINDOWS];
} x11_debug_snapshot_t;

// Fills out_snapshot; returns 1 on success.
int x11_debug_get_snapshot(x11_debug_snapshot_t* out_snapshot);

// ---- Backend runloop + damage (nuts & bolts spine)
void x11_server_runloop_start(void);
void x11_server_runloop_stop(void);
void x11_server_wakeup(void);
  
// Window state (used by damage -> repaint)
void x11_set_window_size(uint32_t xid, int32_t width_px, int32_t height_px);
void x11_mark_damage(uint32_t xid);

// NEW: process one runloop tick (drain damage -> repaint)
void x11_server_step(void);
  
// ---- Optional: dump internal window table for validation
// Writes a human-readable dump into `out` (NUL-terminated). Returns bytes written (excluding NUL).
size_t x11_debug_dump_windows(char* out, size_t out_cap);
  
#ifdef __cplusplus
}
#endif
