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

// Drain pending requests. Must be called on the server/runloop thread.
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

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* x11_requests_h */
