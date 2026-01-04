//
//  x11_backend.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/4/26.
//

#ifndef x11_backend_h
#define x11_backend_h

// x11_backend.h
#include <stdint.h>
#include <stddef.h>
#include "x11_parameters.h"

#ifdef __cplusplus
extern "C" {
#endif

void  x11_backend_init(void);

// Slot helpers (internal only; slot index is NOT stable across calls)
// Returns slot idx if found, else -1.
int   x11_backend_find_slot(uint32_t xid);

// Ensure a slot exists and is marked alive; sets default flags on first creation.
// Returns slot idx on success, else -1.
int   x11_backend_alloc_slot(uint32_t xid);

// Locked variants (caller must hold g_mu)
int   x11_backend_find_slot_locked(uint32_t xid);
int   x11_backend_alloc_slot_locked(uint32_t xid);

// Lifecycle
// Ensure window exists, update size (clamped to >=1), mark damaged.
// Returns slot idx on success, else -1.
int   x11_backend_set_size_and_damage(uint32_t xid, int32_t w_px, int32_t h_px);
int   x11_backend_set_size_and_damage_locked(uint32_t xid, int32_t w_px, int32_t h_px);
// Clears slot, returns detached resources to free OUTSIDE the lock.
// `out_retired` is an opaque pointer to an internal retired-buffer list.
int   x11_backend_window_destroy(uint32_t xid, uint32_t **out_fb, void **out_retired);

// Locked variant (caller must hold g_mu). Detaches resources; caller must free them outside the lock.
int   x11_backend_window_destroy_locked(uint32_t xid, uint32_t **out_fb, void **out_retired);

// Free an opaque retired-buffer list returned by *_window_destroy*.
void  x11_backend_free_retired(void *retired);

// Split lifecycle helpers (preferred going forward)
// Ensure window exists, set size (clamped to >=1). Does NOT mark damaged.
void  x11_backend_window_set_size(uint32_t xid, int32_t w_px, int32_t h_px);
void  x11_backend_window_set_size_locked(uint32_t xid, int32_t w_px, int32_t h_px);

// Damage
// Mark all live windows damaged (useful when presenter becomes available)
void  x11_backend_mark_all_damage(void);
void  x11_backend_mark_all_damage_locked(void);

void  x11_backend_mark_damage(uint32_t xid);
void  x11_backend_mark_damage_locked(uint32_t xid);

int   x11_backend_take_damaged_snapshot(uint32_t *xids, int32_t *ws, int32_t *hs, int cap); // returns n
int   x11_backend_take_damaged_snapshot_locked(uint32_t *xids, int32_t *ws, int32_t *hs, int cap);

// Backing store
int   x11_backend_ensure_fb(uint32_t xid, size_t need_pixels, uint32_t **out_fb); // may grow, returns 1 ok / 0 fail

// Repaint lifecycle
// Snapshot currently-live XIDs into `out_xids` (capacity `cap`), returns count.
int   x11_backend_snapshot_live_xids(uint32_t *out_xids, int cap);
int   x11_backend_snapshot_live_xids_locked(uint32_t *out_xids, int cap);

// Increment/decrement repaint inflight counter and retire old buffers when safe.
// Returns 1 if repaint may proceed, 0 if window is missing/closing.
int   x11_backend_repaint_begin(uint32_t xid);
void  x11_backend_repaint_end(uint32_t xid);


// Locked variants (caller must hold g_mu)
// repaint_end_locked returns an opaque retired-buffer list to free outside the lock.
int   x11_backend_repaint_begin_locked(uint32_t xid);
void  x11_backend_repaint_end_locked(uint32_t xid, void **out_retired);
  
#ifdef __cplusplus
}
#endif

#endif /* x11_backend_h */
