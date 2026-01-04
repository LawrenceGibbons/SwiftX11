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

#include "x11_backend.h"
#include "x11_backend_internal.h"
#include "x11_parameters.h"

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

// Backend truth + lock live here now.
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
x11_win_state_t g_windows[X11_MAX_WINDOWS];

void x11_backend_init(void)
{
  pthread_mutex_lock(&g_mu);
  memset(g_windows, 0, sizeof(g_windows));
  pthread_mutex_unlock(&g_mu);
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
  pthread_mutex_lock(&g_mu);
  idx = x11_backend_find_slot_locked(xid);
  pthread_mutex_unlock(&g_mu);
  return idx;
}

// Public API: ensure a slot exists (allocates if needed).
int x11_backend_alloc_slot(uint32_t xid)
{
  int idx;
  pthread_mutex_lock(&g_mu);
  idx = x11_backend_alloc_slot_locked(xid);
  pthread_mutex_unlock(&g_mu);
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
  pthread_mutex_lock(&g_mu);
  int idx = x11_backend_set_size_and_damage_locked(xid, w_px, h_px);
  pthread_mutex_unlock(&g_mu);
  return idx;
}

int x11_backend_window_destroy(uint32_t xid, uint32_t **out_fb, void **out_retired)
{
  
  if (out_fb) *out_fb = NULL;
  if (out_retired) *out_retired = NULL;

  uint32_t *old_fb = NULL;
  void *retired_opaque = NULL;

  pthread_mutex_lock(&g_mu);
  int ok = x11_backend_window_destroy_locked(xid, &old_fb, &retired_opaque);
  pthread_mutex_unlock(&g_mu);

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

  int idx = x11_backend_alloc_slot_locked(xid);
  if (idx >= 0) {
    g_windows[idx].alive  = 1;
    g_windows[idx].mapped = 1;
    // For set_size we do NOT touch damaged, caller decides.
    // Also: only clear closing if you intend resize to “revive” windows.
    g_windows[idx].closing = 0;
    g_windows[idx].w_px   = w_px;
    g_windows[idx].h_px   = h_px;
  }
}

void x11_backend_window_set_size(uint32_t xid, int32_t w_px, int32_t h_px)
{
  pthread_mutex_lock(&g_mu);
  x11_backend_window_set_size_locked(xid, w_px, h_px);
  pthread_mutex_unlock(&g_mu);
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
    if (g_windows[i].alive && g_windows[i].damaged && !g_windows[i].closing) {
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
  pthread_mutex_lock(&g_mu);
  x11_backend_mark_damage_locked(xid);
  pthread_mutex_unlock(&g_mu);
}

// Mark all live windows as damaged
void x11_backend_mark_all_damage(void)
{
  pthread_mutex_lock(&g_mu);
  x11_backend_mark_all_damage_locked();
  pthread_mutex_unlock(&g_mu);
}

int x11_backend_take_damaged_snapshot(uint32_t *xids, int32_t *ws, int32_t *hs, int cap)
{
  int n;
  pthread_mutex_lock(&g_mu);
  n = x11_backend_take_damaged_snapshot_locked(xids, ws, hs, cap);
  pthread_mutex_unlock(&g_mu);
  return n;
}

// Snapshot all live xids under backend control
int x11_backend_snapshot_live_xids(uint32_t *out, int cap)
{
  int n;
  pthread_mutex_lock(&g_mu);
  n = x11_backend_snapshot_live_xids_locked(out, cap);
  pthread_mutex_unlock(&g_mu);
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
  pthread_mutex_lock(&g_mu);
  idx = x11_backend_find_slot_locked(xid);
  if (idx < 0 || !g_windows[idx].alive) {
    pthread_mutex_unlock(&g_mu);
    return 0;
  }
  fb  = g_windows[idx].fb;
  cap = g_windows[idx].fb_cap_pixels;
  if (cap >= need_pixels && fb) {
    if (out_fb) *out_fb = fb;
    pthread_mutex_unlock(&g_mu);
    return 1;
  }
  pthread_mutex_unlock(&g_mu);

  // Grow outside lock.
  uint32_t *new_fb = (uint32_t*)malloc(need_pixels * sizeof(uint32_t));
  if (!new_fb) return 0;

  uint32_t *old_fb = NULL;
  x11_fb_retired_node_t *to_free = NULL;
  int kept = 0;

  // Swap in under lock if still alive.
  pthread_mutex_lock(&g_mu);
  idx = x11_backend_find_slot_locked(xid);
  if (idx >= 0 && g_windows[idx].alive && g_windows[idx].xid == xid) {
    old_fb = g_windows[idx].fb;
    g_windows[idx].fb = new_fb;
    // After swapping in new_fb...
    // Always detach retired list if inflight is 0 (may free old stuff promptly).
    maybe_detach_retired_locked(idx, &to_free);
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
  
  pthread_mutex_unlock(&g_mu);

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

  x11_fb_retired_node_t *to_free = NULL;
  maybe_detach_retired_locked(idx, &to_free);
  if (out_retired) *out_retired = (void *)to_free;
}


// Public: begin a repaint for a window
int x11_backend_repaint_begin(uint32_t xid)
{
  int ok;
  pthread_mutex_lock(&g_mu);
  ok = x11_backend_repaint_begin_locked(xid);
  pthread_mutex_unlock(&g_mu);
  return ok;
}

void x11_backend_repaint_end(uint32_t xid)
{
  void *retired = NULL;

  pthread_mutex_lock(&g_mu);
  x11_backend_repaint_end_locked(xid, &retired);
  pthread_mutex_unlock(&g_mu);

  if (retired) x11_backend_free_retired(retired);
}

// x11_backend.c
