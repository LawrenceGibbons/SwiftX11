//
//  x11_backend_internal.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/4/26.
//

#pragma once

#ifndef x11_backend_internal_h
#define x11_backend_internal_h

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>

#include "x11_parameters.h"

#ifdef __cplusplus
extern "C" {
#endif

// This header is internal to the X11 backend implementation.
/// This header is internal to the X11 backend implementation.
// It must NOT be included by public headers or external consumers.
// It MAY be included by backend implementation files such as x11_backend.c and x11_shim.c.
//
// Any code including this header is responsible for honoring the backend lock contract:
// - g_mu protects g_windows and all non-atomic fields.
// - repaint_inflight is atomic, but lifecycle transitions still require g_mu.
  
// Keep this internal to the backend (not exported in x11_shim.h)
// One slot per window XID.
// Retired framebuffer list node.
// When we grow/replace fb while repaints are inflight, we must not free the old fb yet.
// We push old pointers here and free them once repaint_inflight reaches 0.
typedef struct x11_fb_retired_node_t {
  uint32_t *ptr;
  struct x11_fb_retired_node_t *next;
} x11_fb_retired_node_t;
  
typedef struct x11_win_state_t {
  // Identity / lifecycle
  uint32_t xid;
  uint8_t  alive;     // slot contains valid window
  uint8_t  mapped;    // MapWindow / UnmapWindow state (rootless visibility; mapped == 1 => visible)
  uint8_t  closing;   // backend wants it gone; used with inflight wait
  uint8_t  _pad0;

  // Hierarchy / attributes (start minimal)
  uint32_t parent_xid;      // 0 = root (we’ll define a fake root)
  uint32_t event_mask;      // SelectInput mask (Core X11 event mask bits)
  uint32_t do_not_propagate;// optional later; keep for correctness path
  uint32_t value_mask;      // optional: which attrs are “set”

  // Geometry (pixels)
  int32_t  x_px;            // top-left in root coords (optional for rootless)
  int32_t  y_px;
  int32_t  w_px;
  int32_t  h_px;
  int32_t  border_px;       // usually 0 for rootless

  // Damage / repaint state
  uint8_t  damaged;         // backend thinks repaint needed
  uint8_t  _pad1[3];

  // Backing store (optional now; you already started this)
  uint32_t *fb;                      // BGRA pixels
  size_t    fb_cap_pixels;           // capacity in pixels
  x11_fb_retired_node_t *fb_retired; // old fb pointers awaiting safe free

  // Concurrency / teardown safety
  _Atomic uint32_t repaint_inflight;  // counts active repaints using the framebuffer pointer
} x11_win_state_t;
  
// globals owned by x11_backend.c
extern pthread_mutex_t g_mu;
extern x11_win_state_t g_windows[X11_MAX_WINDOWS];
  
#ifdef __cplusplus
} // extern "C"
#endif

#endif /* x11_backend_internal_h */
