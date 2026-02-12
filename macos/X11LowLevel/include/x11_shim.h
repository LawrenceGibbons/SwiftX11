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

#include <x11_events.h>

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
int  x11_proto_start_daemon(int display);
void x11_proto_stop_daemon(void);


// ---- Frame presentation (C -> Swift)
typedef void (*x11_present_frame_cb)(
    uint32_t xwin_id,
    const void* bgra,
    int width,
    int height,
    int bytes_per_row
);

//void x11_register_frame_presenter(x11_present_frame_cb on_present);

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
void x11_post_focus_event(uint32_t xid, bool focused);
  
void x11_post_window_raise(uint32_t xid);
void x11_post_window_destroy(uint32_t xid);
void x11_post_window_destroy_async(uint32_t xid);
  

typedef struct {
    uint32_t xid;
    int32_t  w_px;
    int32_t  h_px;
    uint8_t  alive;
    uint8_t  damaged;
    uint16_t _pad;
} x11_debug_window_row_t;

// ---- Backend runloop + damage (nuts & bolts spine)
void x11_server_runloop_start(void);
void x11_server_runloop_stop(void);
void x11_server_wakeup(void);
  
// Window state (used by damage -> repaint)
void x11_set_window_size(uint32_t xid, int32_t width_px, int32_t height_px);
void x11_mark_damage(uint32_t xid);

// Window title related
void x11_window_set_title(uint32_t xid, const char* title_utf8);
  
// NEW: process one runloop tick (drain damage -> repaint)
void x11_server_step(void);
  
// ---- Optional: dump internal window table for validation
// Writes a human-readable dump into `out` (NUL-terminated). Returns bytes written (excluding NUL).
size_t x11_debug_dump_windows(char* out, size_t out_cap);
  
// window create and destroy
uint32_t x11_window_create(const char* title, int32_t w, int32_t h);
void     x11_window_set_title(uint32_t xid, const char* title); // optional
  
// X11 hooks
void x11_apply_window_configure(uint32_t xid, int32_t w_px, int32_t h_px);
  
 // ---- Server-only emit helpers (used by request queue drain)
// These must be called from the server thread (or otherwise safe context).
// Not for Swift/UI
void x11_server_emit_window_create(uint32_t xid,
                                   uint32_t parent_xid, 
                                  const char* title_utf8,
                                  int32_t w_px,
                                  int32_t h_px);

void x11_server_emit_window_destroy(uint32_t xid);

// -------------------------------------------------------------------
// Damage callback from server → shim → Swift
// Called by x11_requests_drain_on_server_thread when a damage arrives.
// Swift implements this via a C bridge function.
void x11_server_emit_window_damage(uint32_t xid);

// -------------------------------------------------------------------
// make the one authoritative bgra copy visible to Swift
//int x11_server_copy_window_bgra(uint32_t xid,
//                                uint8_t* out_bytes,
//                                int32_t out_cap,
//                                int32_t* out_w,
//                                int32_t* out_h,
//                                int32_t* out_bpr);
//  
  
// Swift calls this on the X11 server thread when the host surface resized.
// Only w/h are provided; x/y are unchanged and remain authoritative in C++ WindowTable.
void x11_xproto_apply_rootless_resize_on_server_thread(uint32_t wid, int32_t w_px, int32_t h_px);
  
#ifdef __cplusplus
}
#endif
