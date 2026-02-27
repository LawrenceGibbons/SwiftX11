//
//  x11_requests.h
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/6/26.
//

#pragma once
#ifndef x11_requests_h
#define x11_requests_h

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Client-style API (preferred)
// These are the functions Swift/UI should call going forward.
// Today they enqueue X11-like requests.

typedef struct {
  int32_t w_px;
  int32_t h_px;
} x11_client_geom_t;

// Create a new X11 window on the server.
// Returns xid (0 on failure).
uint32_t x11_client_create_window(const char* title_utf8, int32_t w_px, int32_t h_px);

// Destroy is "client request destroy".
void x11_client_destroy_window(uint32_t xid);
void x11_client_destroy_window_async(uint32_t xid);

// Map/unmap are client requests (X11-ish).
void x11_client_map_window(uint32_t xid);
void x11_client_unmap_window(uint32_t xid);

// Configure (resize for now; later x/y too).
void x11_client_configure_window(uint32_t xid, int32_t w_px, int32_t h_px);

// Properties
void x11_client_set_window_title(uint32_t xid, const char* title_utf8);

// Drain pending client-style requests. Must be called on the server/runloop thread.
void x11_requests_drain_on_server_thread(void);

// resize when Cocoa side changes window size  
int x11_requests_push_rootless_resize(uint32_t xid, int32_t w_px, int32_t h_px);

// ---- Server-queue push APIs (used by x11_xproto thread)
// These enqueue requests onto the same client->server request queue.
// Return 1 on success, 0 if dropped.
int x11_requests_push_create(uint32_t xid, uint32_t parent_xid, const char* title_utf8, int32_t w_px, int32_t h_px);
int x11_requests_push_destroy(uint32_t xid);
int x11_requests_push_map(uint32_t xid);
int x11_requests_push_unmap(uint32_t xid);
int x11_requests_push_configure(uint32_t xid, int32_t w_px, int32_t h_px);
int x11_requests_push_set_title(uint32_t xid, const char* title_utf8);
int x11_requests_push_window_presentable(uint32_t xid);
bool x11_requests_push_set_cursor(uint32_t host_xid, uint32_t cursor_xid);
  
//int x11_backend_copy_window_bgra(uint32_t xid,
//                                 uint8_t* out_bytes,
//                                 int32_t out_cap,
//                                 int32_t* out_w,
//                                 int32_t* out_h,
//                                 int32_t* out_bpr);
//  
#ifdef __cplusplus
} // extern "C"
#endif

#endif /* x11_requests_h */
