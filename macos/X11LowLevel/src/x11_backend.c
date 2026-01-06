//
//  x11_backend.c
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/4/26.
//

#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

#include "x11_backend.h"
#include "x11_backend_internal.h"
#include "x11_parameters.h"

// Backend truth + lock live here now.
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static x11_win_state_t g_windows[X11_MAX_WINDOWS];

static pthread_cond_t g_inflight_cv;
static int g_inflight_cv_inited = 0;

// Helper to free a retired fb list.
static void free_retired_list(x11_fb_retired_node_t *node) {
  while (node) {
    x11_fb_retired_node_t *next = node->next;
    if (node->ptr) free(node->ptr);
    free(node);
    node = next;
  }
}

// Public API: free an opaque retired-fb list returned by destroy.
void x11_backend_free_retired(void *retired_opaque)
{
  if (!retired_opaque) return;
  free_retired_list((x11_fb_retired_node_t *)retired_opaque);
}

static void maybe_detach_retired_locked(int idx, x11_fb_retired_node_t **out_list) {
  if (out_list) *out_list = NULL;
  if (idx < 0) return;
  if (atomic_load_explicit(&g_windows[idx].repaint_inflight, memory_order_relaxed) != 0) return;
  if (!g_windows[idx].fb_retired) return;
  if (out_list) {
    *out_list = g_windows[idx].fb_retired;
  }
  g_windows[idx].fb_retired = NULL;
}

void x11_backend_lock(void)
{
  pthread_mutex_lock(&g_mu);
}

void x11_backend_unlock(void)
{
  pthread_mutex_unlock(&g_mu);
}

void x11_backend_cond_wait(pthread_cond_t *cv)
{
  if (!cv) return;
  (void)pthread_cond_wait(cv, &g_mu);
}

int x11_backend_cond_timedwait(pthread_cond_t *cv, const struct timespec *abstime)
{
  if (!cv || !abstime) return -1;
  return pthread_cond_timedwait(cv, &g_mu, abstime);
}

void x11_backend_cond_signal(pthread_cond_t *cv)
{
  if (!cv) return;
  (void)pthread_cond_signal(cv);
}

void x11_backend_cond_broadcast(pthread_cond_t *cv)
{
  if (!cv) return;
  (void)pthread_cond_broadcast(cv);
}


void x11_backend_inflight_cv_init(void)
{
  if (g_inflight_cv_inited) return;

  pthread_condattr_t attr;
  int rc = pthread_condattr_init(&attr);
#ifndef NDEBUG
  assert(rc == 0);
#else
  (void)rc;
#endif

  rc = pthread_cond_init(&g_inflight_cv, &attr);
#ifndef NDEBUG
  assert(rc == 0);
#else
  (void)rc;
#endif

  pthread_condattr_destroy(&attr);
  g_inflight_cv_inited = 1;
}

void x11_backend_inflight_cv_shutdown(void)
{
  if (!g_inflight_cv_inited) return;
  pthread_cond_destroy(&g_inflight_cv);
  g_inflight_cv_inited = 0;
}


void x11_backend_shutdown(void)
{
  x11_backend_lock();
  // optional: clear tables
  memset(g_windows, 0, sizeof(g_windows));
  // destroy inflight CV if initialized
  x11_backend_inflight_cv_shutdown();
  x11_backend_unlock();
}


void x11_backend_clear_windows(void)
{
  x11_backend_lock();
  memset(g_windows, 0, sizeof(g_windows));
  x11_backend_unlock();
}


void x11_backend_init(void)
{
  x11_backend_lock();

  x11_backend_inflight_cv_init();
  
  x11_backend_unlock();
}

// ---- Slot helpers
// Contract: *_locked variants require the caller to hold g_mu.

int x11_backend_find_slot_locked(uint32_t xid)
{
  for (int i = 0; i < X11_MAX_WINDOWS; i++) {
    if (g_windows[i].alive && g_windows[i].xid == xid) return i;
  }
  return -1;
}

int x11_backend_alloc_slot_locked(uint32_t xid)
{
  // If it already exists, return it.
  for (int i = 0; i < X11_MAX_WINDOWS; i++) {
    if (g_windows[i].alive && g_windows[i].xid == xid) return i;
  }

  // Otherwise, find a free slot.
  for (int i = 0; i < X11_MAX_WINDOWS; i++) {
    if (!g_windows[i].alive) {
      // Clear everything to keep invariants sane (xid=0 when dead, fb NULL, etc.)
      memset(&g_windows[i], 0, sizeof(g_windows[i]));

      g_windows[i].xid     = xid;
      g_windows[i].alive   = 1;
      g_windows[i].mapped  = 1;   // rootless default visible
      g_windows[i].closing = 0;
      // repaint_inflight atomic is already zero due to memset

      return i;
    }
  }
  return -1; // no slots left
}


// ---- Backend-internal helpers used by x11_shim.c (caller must hold g_mu)
int x11_backend_window_is_alive_locked(uint32_t xid)
{
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return 0;
  return g_windows[idx].alive ? 1 : 0;
}

int x11_backend_window_is_closing_locked(uint32_t xid)
{
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return 0;
  return g_windows[idx].closing ? 1 : 0;
}

uint32_t x11_backend_repaint_inflight_locked(uint32_t xid)
{
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return 0;
  return atomic_load_explicit(&g_windows[idx].repaint_inflight, memory_order_relaxed);
}


int x11_backend_wait_inflight_zero_locked(uint32_t xid)
{
  // Caller must hold backend lock.
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return 0;

  while (atomic_load_explicit(&g_windows[idx].repaint_inflight, memory_order_relaxed) != 0) {
    if (g_inflight_cv_inited) {
      x11_backend_cond_wait(&g_inflight_cv);

      // Defensive: slot could disappear or be reused.
      idx = x11_backend_find_slot_locked(xid);
      if (idx < 0) return 0;
    } else {
      // Fallback: briefly yield without holding the lock.
      x11_backend_unlock();
      usleep(1000);
      x11_backend_lock();

      idx = x11_backend_find_slot_locked(xid);
      if (idx < 0) return 0;
    }
  }

  return 1;
}


int x11_backend_window_begin_close_locked(uint32_t xid)
{
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return 0;
  g_windows[idx].closing = 1;
  g_windows[idx].damaged = 0;
  return 1;
}

int x11_backend_window_exists_locked(uint32_t xid)
{
  return (x11_backend_find_slot_locked(xid) >= 0) ? 1 : 0;
}


int x11_backend_get_fb_locked(uint32_t xid, uint32_t **out_fb)
{
  if (out_fb) *out_fb = NULL;
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return 0;
  if (!g_windows[idx].alive) return 0;
  if (out_fb) *out_fb = g_windows[idx].fb;
  return 1;
}

#ifndef NDEBUG


static void debug_check_slot_invariants_locked(int i)
{
  x11_win_state_t *w = &g_windows[i];

  if (!w->alive) {
    assert(w->xid == 0);
    assert(w->w_px == 0);
    assert(w->h_px == 0);
    assert(w->damaged == 0);
    assert(w->closing == 0);
    assert(w->fb == NULL);
    assert(w->fb_cap_pixels == 0);
    assert(atomic_load_explicit(&w->repaint_inflight, memory_order_relaxed) == 0);
    return;
  }

  assert(w->xid != 0);
  assert(w->w_px >= 1);
  assert(w->h_px >= 1);

  if (w->closing) {
    assert(w->damaged == 0);
  }

  if (w->fb == NULL) {
    assert(w->fb_cap_pixels == 0);
  } else {
    assert(w->fb_cap_pixels > 0);
  }
}

void x11_backend_debug_check_all_invariants_locked(void)
{
  for (int i = 0; i < X11_MAX_WINDOWS; i++) {
    debug_check_slot_invariants_locked(i);
  }
}

void x11_backend_debug_check_invariants_for_xid_locked(uint32_t xid)
{
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return;
  debug_check_slot_invariants_locked(idx);
}

int x11_backend_debug_snapshot_windows_locked(x11_backend_debug_win_t *out_wins, int cap)
{
  if (!out_wins || cap <= 0) return 0;
  int n = 0;
  for (int i = 0; i < X11_MAX_WINDOWS && n < cap; i++) {
    if (!g_windows[i].alive) continue;
    out_wins[n].xid      = g_windows[i].xid;
    out_wins[n].w_px     = g_windows[i].w_px;
    out_wins[n].h_px     = g_windows[i].h_px;
    out_wins[n].alive    = g_windows[i].alive;
    out_wins[n].damaged  = g_windows[i].damaged;
    out_wins[n].closing  = g_windows[i].closing;
    out_wins[n].inflight =
      atomic_load_explicit(&g_windows[i].repaint_inflight, memory_order_relaxed);
    n++;
  }
  return n;
}
#endif


int x11_backend_get_size_locked(uint32_t xid, int32_t *out_w_px, int32_t *out_h_px)
{
  if (out_w_px) *out_w_px = 0;
  if (out_h_px) *out_h_px = 0;

  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0 || !g_windows[idx].alive) return 0;

  if (out_w_px) *out_w_px = g_windows[idx].w_px;
  if (out_h_px) *out_h_px = g_windows[idx].h_px;
  return 1;
}

int x11_backend_get_damaged_locked(uint32_t xid, uint8_t *out_damaged)
{
  if (out_damaged) *out_damaged = 0;
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0 || !g_windows[idx].alive) return 0;
  if (out_damaged) *out_damaged = g_windows[idx].damaged;
  return 1;
}

int x11_backend_window_can_repaint_locked(uint32_t xid)
{
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0 || !g_windows[idx].alive) return 0;
  if (g_windows[idx].closing) return 0;
  if (!g_windows[idx].mapped) return 0;
  return 1;
}

// Locked destroy helper used by the public wrapper.
int x11_backend_window_destroy_locked(uint32_t xid,
                                     uint32_t **out_fb,
                                     void **out_retired)
{
  if (out_fb) *out_fb = NULL;
  if (out_retired) *out_retired = NULL;

  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return 0;

  // HARD INVARIANT: never destroy while repaints are in-flight
  if (atomic_load_explicit(&g_windows[idx].repaint_inflight, memory_order_relaxed) != 0) {
    return 0;
  }
  
  // Detach backing store pointers so caller can free after unlock.
  uint32_t *old_fb = g_windows[idx].fb;
  x11_fb_retired_node_t *retired = g_windows[idx].fb_retired;

  g_windows[idx].fb = NULL;
  g_windows[idx].fb_cap_pixels = 0;
  g_windows[idx].fb_retired = NULL;

  // Clear the slot.
  g_windows[idx].alive   = 0;
  g_windows[idx].mapped  = 0;
  g_windows[idx].damaged = 0;
  g_windows[idx].xid     = 0;
  g_windows[idx].w_px    = 0;
  g_windows[idx].h_px    = 0;
  g_windows[idx].closing = 0;   // invariant: dead slots must have closing=0

  if (out_fb) *out_fb = old_fb;
  if (out_retired) *out_retired = (void *)retired;
  return 1;
}

// Back-compat name (TAKES the lock).
int x11_backend_find_slot(uint32_t xid)
{
  int idx;
  x11_backend_lock();
  idx = x11_backend_find_slot_locked(xid);
  x11_backend_unlock();
  return idx;
}

// Public API: ensure a slot exists (allocates if needed).
int x11_backend_alloc_slot(uint32_t xid)
{
  int idx;
  x11_backend_lock();
  idx = x11_backend_alloc_slot_locked(xid);
  x11_backend_unlock();
  return idx;
}

// ---- Lifecycle
// Locked variant: caller must hold g_mu
int x11_backend_set_size_and_damage_locked(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  int idx = x11_backend_alloc_slot_locked(xid);
  if (idx >= 0) {
    g_windows[idx].alive   = 1;
    g_windows[idx].mapped  = 1;
    g_windows[idx].closing = 0;
    g_windows[idx].w_px    = w_px;
    g_windows[idx].h_px    = h_px;
    g_windows[idx].damaged = 1;
  }
  return idx;
}

// Public wrapper: takes lock
int x11_backend_set_size_and_damage(uint32_t xid, int32_t w_px, int32_t h_px)
{
  x11_backend_lock();
  int idx = x11_backend_set_size_and_damage_locked(xid, w_px, h_px);
  x11_backend_unlock();
  return idx;
}

int x11_backend_window_destroy(uint32_t xid, uint32_t **out_fb, void **out_retired)
{
  
  if (out_fb) *out_fb = NULL;
  if (out_retired) *out_retired = NULL;

  uint32_t *old_fb = NULL;
  void *retired_opaque = NULL;

  x11_backend_lock();
  int ok = x11_backend_window_destroy_locked(xid, &old_fb, &retired_opaque);
  x11_backend_unlock();

  if (!ok) return 0;

  if (out_fb) *out_fb = old_fb;
  if (out_retired) *out_retired = retired_opaque;
  return 1;
}

// ---- Damage (locked variants)
// Contract: caller must hold g_mu.
void x11_backend_mark_damage_locked(uint32_t xid)
{
  int idx = x11_backend_find_slot_locked(xid);
  if (idx >= 0 && g_windows[idx].alive && !g_windows[idx].closing) {
    g_windows[idx].damaged = 1;
  }
}

void x11_backend_window_set_size_locked(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  // IMPORTANT: do not allocate here; only create-paths allocate.
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return;
  if (!g_windows[idx].alive) return;

  // Keep mapped consistent with “rootless visible” default.
  g_windows[idx].mapped = 1;

  // Do NOT clear closing here; resize must not “revive” a window mid-destroy.
  g_windows[idx].w_px = w_px;
  g_windows[idx].h_px = h_px;
}


void x11_backend_window_set_size(uint32_t xid, int32_t w_px, int32_t h_px)
{
  x11_backend_lock();
  x11_backend_window_set_size_locked(xid, w_px, h_px);
  x11_backend_unlock();
}


void x11_backend_mark_all_damage_locked(void)
{
  for (int i = 0; i < X11_MAX_WINDOWS; i++) {
    if (g_windows[i].alive && !g_windows[i].closing) {
      g_windows[i].damaged = 1;
    }
  }
}

int x11_backend_take_damaged_snapshot_locked(uint32_t *xids, int32_t *ws, int32_t *hs, int cap)
{
  if (!xids || !ws || !hs || cap <= 0) return 0;
  int n = 0;
  for (int i = 0; i < X11_MAX_WINDOWS; i++) {
    if (g_windows[i].alive && g_windows[i].mapped &&
        g_windows[i].damaged && !g_windows[i].closing) 
    {
      if (n >= cap) break;
      // Clear damage now that we are committing to a repaint.
      g_windows[i].damaged = 0;
      xids[n] = g_windows[i].xid;
      ws[n]   = g_windows[i].w_px;
      hs[n]   = g_windows[i].h_px;
      n++;
    }
  }
  return n;
}

int x11_backend_snapshot_live_xids_locked(uint32_t *out, int cap)
{
  if (!out || cap <= 0) return 0;
  int n = 0;
  for (int i = 0; i < X11_MAX_WINDOWS && n < cap; i++) {
    if (g_windows[i].alive) {
      out[n++] = g_windows[i].xid;
    }
  }
  return n;
}

void x11_backend_mark_damage(uint32_t xid)
{
  x11_backend_lock();
  x11_backend_mark_damage_locked(xid);
  x11_backend_unlock();
}

// Mark all live windows as damaged
void x11_backend_mark_all_damage(void)
{
  x11_backend_lock();
  x11_backend_mark_all_damage_locked();
  x11_backend_unlock();
}

int x11_backend_take_damaged_snapshot(uint32_t *xids, int32_t *ws, int32_t *hs, int cap)
{
  int n;
  x11_backend_lock();
  n = x11_backend_take_damaged_snapshot_locked(xids, ws, hs, cap);
  x11_backend_unlock();
  return n;
}

// Snapshot all live xids under backend control
int x11_backend_snapshot_live_xids(uint32_t *out, int cap)
{
  int n;
  x11_backend_lock();
  n = x11_backend_snapshot_live_xids_locked(out, cap);
  x11_backend_unlock();
  return n;
}

// ---- Backing store
int x11_backend_ensure_fb(uint32_t xid, size_t need_pixels, uint32_t **out_fb)
{
  if (out_fb) *out_fb = NULL;
  if (need_pixels == 0) return 0;

  int idx;
  uint32_t *fb = NULL;
  size_t cap = 0;

  // Fast path: check existing capacity under lock.
  x11_backend_lock();
  idx = x11_backend_find_slot_locked(xid);
  if (idx < 0 || !g_windows[idx].alive) {
    x11_backend_unlock();
    return 0;
  }
  fb  = g_windows[idx].fb;
  cap = g_windows[idx].fb_cap_pixels;
  if (cap >= need_pixels && fb) {
    if (out_fb) *out_fb = fb;
    x11_backend_unlock();
    return 1;
  }
  x11_backend_unlock();

  // Grow outside lock.
  uint32_t *new_fb = (uint32_t*)malloc(need_pixels * sizeof(uint32_t));
  if (!new_fb) return 0;

  uint32_t *old_fb = NULL;
  x11_fb_retired_node_t *to_free = NULL;
  int kept = 0;

  // Swap in under lock if still alive.
  x11_backend_lock();
  idx = x11_backend_find_slot_locked(xid);
  if (idx >= 0 && g_windows[idx].alive && g_windows[idx].xid == xid) {
    old_fb = g_windows[idx].fb;
    g_windows[idx].fb = new_fb;
    g_windows[idx].fb_cap_pixels = need_pixels;
    if (out_fb) *out_fb = new_fb;
    kept = 1;

    // Retire the old buffer if it exists.
    if (old_fb) {
      if (atomic_load_explicit(&g_windows[idx].repaint_inflight, memory_order_relaxed) != 0) {
        // Not safe: put old_fb on the retired list.
        x11_fb_retired_node_t *node = (x11_fb_retired_node_t*)malloc(sizeof(x11_fb_retired_node_t));
        if (node) {
          node->ptr = old_fb;
          node->next = g_windows[idx].fb_retired;
          g_windows[idx].fb_retired = node;
          old_fb = NULL; // ownership moved to retired list
        } else {
          // leak rather than UAF
          old_fb = NULL;
        }
      }
    }
    // Detach any retired list if inflight is 0
    maybe_detach_retired_locked(idx, &to_free);
  }
  
  x11_backend_unlock();

  if (!kept) {
    free(new_fb);
    return 0;
  }

  // Free any buffers we detached safely.
  if (old_fb) free(old_fb);
  if (to_free) free_retired_list(to_free);
  return 1;
}


// ---- Repaint inflight tracking (locked variants)
// Contract: caller must hold g_mu.
int x11_backend_repaint_begin_locked(uint32_t xid)
{
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return 0;
  if (!g_windows[idx].alive) return 0;
  if (g_windows[idx].closing) return 0;
  atomic_fetch_add_explicit(&g_windows[idx].repaint_inflight, 1, memory_order_relaxed);
  return 1;
}


// Decrements inflight (if >0) and detaches any now-safe retired FB list.
// Returns a list to free outside the lock via *out_to_free.
void x11_backend_repaint_end_locked(uint32_t xid, void **out_retired)
{
  if (out_retired) *out_retired = NULL;

  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return;

  uint32_t v = atomic_load_explicit(&g_windows[idx].repaint_inflight, memory_order_relaxed);
  if (v != 0) {
    (void)atomic_fetch_sub_explicit(&g_windows[idx].repaint_inflight, 1, memory_order_relaxed);
  }

  // If a destroy is waiting, wake it when we hit zero inflight on a closing window.
  if (g_inflight_cv_inited &&
      g_windows[idx].closing &&
      atomic_load_explicit(&g_windows[idx].repaint_inflight, memory_order_relaxed) == 0)
  {
    x11_backend_cond_broadcast(&g_inflight_cv);
  }

  x11_fb_retired_node_t *to_free = NULL;
  maybe_detach_retired_locked(idx, &to_free);
  if (out_retired) *out_retired = (void *)to_free;
}


// Public: begin a repaint for a window
int x11_backend_repaint_begin(uint32_t xid)
{
  int ok;
  x11_backend_lock();
  ok = x11_backend_repaint_begin_locked(xid);
  x11_backend_unlock();
  return ok;
}

void x11_backend_repaint_end(uint32_t xid)
{
  void *retired = NULL;

  x11_backend_lock();
  x11_backend_repaint_end_locked(xid, &retired);
  x11_backend_unlock();

  if (retired) x11_backend_free_retired(retired);
}


int x11_backend_window_set_mapped_locked(uint32_t xid, int mapped)
{
  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return 0;
  if (!g_windows[idx].alive) return 0;

  g_windows[idx].mapped = mapped ? 1 : 0;

  // If we are unmapping, ensure we don't keep generating repaints.
  if (!g_windows[idx].mapped) {
    g_windows[idx].damaged = 0;
  }
  return 1;
}

// x11_backend.c
