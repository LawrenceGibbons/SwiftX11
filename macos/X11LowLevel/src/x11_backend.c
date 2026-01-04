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

void x11_backend_repaint_finished_locked(uint32_t xid, x11_fb_retired_node_t **out_to_free)
{
  if (out_to_free) *out_to_free = NULL;
  int idx = x11_backend_find_slot_locked(xid);
  if (idx >= 0) {
    maybe_detach_retired_locked(idx, out_to_free);
  }
}


// Back-compat names (NO internal locking). Prefer *_locked in new code.
int x11_backend_find_slot(uint32_t xid)
{
  int idx;
  pthread_mutex_lock(&g_mu);
  idx = x11_backend_find_slot_locked(xid);
  pthread_mutex_unlock(&g_mu);
  return idx;
}

int x11_backend_alloc_slot(uint32_t xid)
{
  int idx;
  pthread_mutex_lock(&g_mu);
  idx = x11_backend_alloc_slot_locked(xid);
  pthread_mutex_unlock(&g_mu);
  return idx;
}

// ---- Lifecycle
int x11_backend_window_create(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  pthread_mutex_lock(&g_mu);
  int idx = x11_backend_alloc_slot_locked(xid);
  if (idx >= 0) {
    g_windows[idx].alive   = 1;
    g_windows[idx].mapped  = 1;
    g_windows[idx].closing = 0;
    g_windows[idx].w_px    = w_px;
    g_windows[idx].h_px    = h_px;
    g_windows[idx].damaged = 1;
  }
  pthread_mutex_unlock(&g_mu);
  return idx;
}

int x11_backend_window_destroy(uint32_t xid, uint32_t **out_fb)
{
  if (out_fb) *out_fb = NULL;

  uint32_t *old_fb = NULL;
  x11_fb_retired_node_t *retired = NULL;

  pthread_mutex_lock(&g_mu);
  int ok = x11_backend_window_destroy_locked(xid, &old_fb, &retired);
  pthread_mutex_unlock(&g_mu);

  if (!ok) return 0;

  if (out_fb) *out_fb = old_fb;
  if (retired) x11_backend_free_retired_list(retired);
  return 1;
}

// ---- Damage
void x11_backend_mark_damage(uint32_t xid)
{
  pthread_mutex_lock(&g_mu);
  int idx = x11_backend_find_slot_locked(xid);
  if (idx >= 0 && g_windows[idx].alive) {
    g_windows[idx].damaged = 1;
  }
  pthread_mutex_unlock(&g_mu);
}

int x11_backend_take_damaged_snapshot(uint32_t *xids, int32_t *ws, int32_t *hs, int cap)
{
  int n = 0;
  pthread_mutex_lock(&g_mu);
  for (int i = 0; i < X11_MAX_WINDOWS; i++) {
    if (g_windows[i].alive && g_windows[i].damaged) {
      if (n >= cap) break;
      g_windows[i].damaged = 0;
      xids[n] = g_windows[i].xid;
      ws[n]   = g_windows[i].w_px;
      hs[n]   = g_windows[i].h_px;
      n++;
    }
  }
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
    g_windows[idx].fb_cap_pixels = need_pixels;
    if (out_fb) *out_fb = new_fb;
    kept = 1;

    // Retire the old buffer if it exists.
    if (old_fb) {
      if (atomic_load_explicit(&g_windows[idx].repaint_inflight, memory_order_relaxed) == 0) {
        // Safe to free immediately (no inflight repaints).
        // Also detach any previously-retired buffers.
        maybe_detach_retired_locked(idx, &to_free);
      } else {
        // Not safe: put old_fb on the retired list.
        x11_fb_retired_node_t *node = (x11_fb_retired_node_t*)malloc(sizeof(x11_fb_retired_node_t));
        if (node) {
          node->ptr = old_fb;
          node->next = g_windows[idx].fb_retired;
          g_windows[idx].fb_retired = node;
          old_fb = NULL; // ownership moved to retired list
        }
        // If node allocation failed, we intentionally leak old_fb rather than UAF.
      }
    }

    // If inflight is already 0, we can also detach any retired list now.
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

void x11_backend_repaint_finished(uint32_t xid)
{
  x11_fb_retired_node_t *to_free = NULL;

  pthread_mutex_lock(&g_mu);
  x11_backend_repaint_finished_locked(xid, &to_free);
  pthread_mutex_unlock(&g_mu);

  if (to_free) free_retired_list(to_free);
}

// NEW: non-static wrapper usable from shim (or other backend code)
void x11_backend_free_retired_list(x11_fb_retired_node_t *node)
{
  free_retired_list(node);
}

// x11_backend.c

int x11_backend_window_destroy_locked(uint32_t xid,
                                      uint32_t **out_fb,
                                      x11_fb_retired_node_t **out_retired)
{
  if (out_fb) *out_fb = NULL;
  if (out_retired) *out_retired = NULL;

  int idx = x11_backend_find_slot_locked(xid);
  if (idx < 0) return 0;

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

  // IMPORTANT: repaint_inflight must already be 0 at this point,
  // OR you must have waited in the caller before calling destroy_locked.
  // (Your shim currently waits — good.)

  if (out_fb) *out_fb = old_fb;
  if (out_retired) *out_retired = retired;
  return 1;
}
