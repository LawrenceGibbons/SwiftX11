//
//  x11_backend.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/4/26.
//

#ifndef x11_backend_h
#define x11_backend_h

// x11_backend.h
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "x11_parameters.h"

#ifdef __cplusplus
extern "C" {
#endif

void  x11_backend_init(void);

// Slot queries
int   x11_backend_find_slot(uint32_t xid);          // returns idx or -1
int   x11_backend_alloc_slot(uint32_t xid);         // returns idx or -1

// Lifecycle
// Existing non-locked destroy can for callers that don't hold g_mu.
int  x11_backend_window_create(uint32_t xid, int32_t w_px, int32_t h_px); // ensures slot + sets alive + size
int   x11_backend_window_destroy(uint32_t xid, uint32_t **out_fb);         // clears slot, returns old fb to free

// Damage
void  x11_backend_mark_damage(uint32_t xid);
int   x11_backend_take_damaged_snapshot(uint32_t *xids, int32_t *ws, int32_t *hs, int cap); // returns n

// Backing store
int   x11_backend_ensure_fb(uint32_t xid, size_t need_pixels, uint32_t **out_fb); // may grow, returns 1 ok / 0 fail

// Repaint lifecycle
void  x11_backend_repaint_finished(uint32_t xid);

#ifdef __cplusplus
}
#endif

#endif /* x11_backend_h */
