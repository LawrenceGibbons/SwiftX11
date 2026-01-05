//
//  x11_backend.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/4/26.
//

#ifndef x11_backend_h
#define x11_backend_h

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>

#include "x11_parameters.h"

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Backend global init
// -----------------------------------------------------------------------------
void  x11_backend_init(void);

// -----------------------------------------------------------------------------
// Backend lock boundary (preferred in shim instead of touching g_mu)
// -----------------------------------------------------------------------------
void  x11_backend_lock(void);
void  x11_backend_unlock(void);

// Condition-variable helpers that wait/signal while holding the backend lock.
// (Lets x11_shim.c use pthread_cond_t without including backend_internal.h.)
void  x11_backend_cond_wait(pthread_cond_t *cv);
int   x11_backend_cond_timedwait(pthread_cond_t *cv, const struct timespec *abstime);
void  x11_backend_cond_signal(pthread_cond_t *cv);
void  x11_backend_cond_broadcast(pthread_cond_t *cv);

// -----------------------------------------------------------------------------
// Slot helpers (internal only; slot index is NOT stable across calls)
// -----------------------------------------------------------------------------
int   x11_backend_find_slot(uint32_t xid);
int   x11_backend_alloc_slot(uint32_t xid);

// Locked variants (caller must hold backend lock via x11_backend_lock())
int   x11_backend_find_slot_locked(uint32_t xid);
int   x11_backend_alloc_slot_locked(uint32_t xid);

// -----------------------------------------------------------------------------
// Backend state accessors (caller must hold backend lock)
// -----------------------------------------------------------------------------

// Returns 1 if window exists and is alive (NOTE: does not imply repaint-safe).
int      x11_backend_window_is_alive_locked(uint32_t xid);

// Returns 1 if window exists and is closing, else 0.
int      x11_backend_window_is_closing_locked(uint32_t xid);

// Returns repaint_inflight count if window exists, else 0.
uint32_t x11_backend_repaint_inflight_locked(uint32_t xid);

// Returns 1 if window exists, alive, and not closing (safe to repaint).
int      x11_backend_window_can_repaint_locked(uint32_t xid);

// If window exists, writes current fb pointer to *out_fb (may be NULL) and returns 1.
int      x11_backend_get_fb_locked(uint32_t xid, uint32_t **out_fb);

// If window exists, writes size to *out_w_px/*out_h_px and returns 1.
int      x11_backend_get_size_locked(uint32_t xid, int32_t *out_w_px, int32_t *out_h_px);

// If window exists, writes damaged flag (0/1) to *out_damaged and returns 1.
int      x11_backend_get_damaged_locked(uint32_t xid, uint8_t *out_damaged);

// Sets closing=1 and clears damaged=0. Returns 1 if window exists, else 0.
int      x11_backend_window_begin_close_locked(uint32_t xid);

#ifndef NDEBUG
// -----------------------------------------------------------------------------
// Debug snapshot helpers (caller must hold backend lock)
// -----------------------------------------------------------------------------
typedef struct x11_backend_debug_win_t {
  uint32_t xid;
  int32_t  w_px;
  int32_t  h_px;
  uint8_t  alive;
  uint8_t  damaged;
  uint8_t  closing;
  uint8_t  _pad;
  uint32_t inflight;
} x11_backend_debug_win_t;

// Debug-only: validate backend invariants for a specific xid (must hold backend lock).
void     x11_backend_debug_check_invariants_for_xid_locked(uint32_t xid);

// Debug-only: validate invariants for all slots (must hold backend lock).
void     x11_backend_debug_check_all_invariants_locked(void);

// Debug-only: snapshot all live windows into out_wins (must hold backend lock), returns count.
int      x11_backend_debug_snapshot_windows_locked(x11_backend_debug_win_t *out_wins, int cap);
#endif

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------
int   x11_backend_set_size_and_damage(uint32_t xid, int32_t w_px, int32_t h_px);
int   x11_backend_set_size_and_damage_locked(uint32_t xid, int32_t w_px, int32_t h_px);

int   x11_backend_window_destroy(uint32_t xid, uint32_t **out_fb, void **out_retired);
int   x11_backend_window_destroy_locked(uint32_t xid, uint32_t **out_fb, void **out_retired);

void  x11_backend_free_retired(void *retired);

// Split lifecycle helpers (preferred going forward)
void  x11_backend_window_set_size(uint32_t xid, int32_t w_px, int32_t h_px);
void  x11_backend_window_set_size_locked(uint32_t xid, int32_t w_px, int32_t h_px);

// -----------------------------------------------------------------------------
// Damage
// -----------------------------------------------------------------------------
void  x11_backend_mark_all_damage(void);
void  x11_backend_mark_all_damage_locked(void);

void  x11_backend_mark_damage(uint32_t xid);
void  x11_backend_mark_damage_locked(uint32_t xid);

int   x11_backend_take_damaged_snapshot(uint32_t *xids, int32_t *ws, int32_t *hs, int cap);
int   x11_backend_take_damaged_snapshot_locked(uint32_t *xids, int32_t *ws, int32_t *hs, int cap);

// -----------------------------------------------------------------------------
// Backing store
// -----------------------------------------------------------------------------
int   x11_backend_ensure_fb(uint32_t xid, size_t need_pixels, uint32_t **out_fb);

// -----------------------------------------------------------------------------
// Repaint lifecycle
// -----------------------------------------------------------------------------
int   x11_backend_snapshot_live_xids(uint32_t *out_xids, int cap);
int   x11_backend_snapshot_live_xids_locked(uint32_t *out_xids, int cap);

int   x11_backend_repaint_begin(uint32_t xid);
void  x11_backend_repaint_end(uint32_t xid);

int   x11_backend_repaint_begin_locked(uint32_t xid);
void  x11_backend_repaint_end_locked(uint32_t xid,
                                     pthread_cond_t *inflight_cv,
                                     int inflight_cv_inited,
                                     void **out_retired);
  
int   x11_backend_wait_inflight_zero_locked(uint32_t xid,
                                            pthread_cond_t *inflight_cv,
                                            int inflight_cv_inited);
#ifdef __cplusplus
}
#endif

#endif /* x11_backend_h */
