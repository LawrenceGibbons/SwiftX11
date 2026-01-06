//
//  x11_requests.c
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/6/26.
// This file emits server → client events and bridges to Swift.
//

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <time.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdarg.h>

#include "x11_parameters.h"
#include "x11_shim.h"
#include "x11_events.h"   // so we can enqueue x11_event_t
#include "x11_backend.h"
#include "x11_requests.h"
#include "x11_server_internal.h"


// For window creation and handling
static _Atomic uint32_t g_next_xid = 0x10010; // avoid colliding with old demo ids


// -----------------------------------------------------------------------------
// Shim-local server context (scaffold step): collect shim state into one struct.
// Protected by the backend lock (x11_backend_lock/unlock).
// -----------------------------------------------------------------------------
typedef struct x11_server_ctx_t {
  // Callbacks
  x11_window_created_cb on_create;
  x11_window_closed_cb  on_close;
  x11_present_frame_cb  present;

  // ---- Runloop thread + wakeup
  pthread_t      thread;
  pthread_cond_t cv;
  int            runloop_cv_inited;

  int runloop_running;
  int runloop_stop;

  // ---- Routing state
  uint32_t pointer_xid;
  uint32_t focus_xid;
  uint32_t drag_xid;
  uint32_t buttons;   // current button mask

  // ---- Debug: repaint storm injector (useful for stop/destroy race testing)
  int      debug_storm;
  uint32_t debug_storm_xid;
  int      debug_destroy_during_repaint;
  uint32_t debug_destroy_during_repaint_xid;

  // Debug: count how many destroys had to wait for in-flight repaints
  _Atomic uint64_t debug_destroy_waits;
  _Atomic uint64_t dbg_destroy_noop;

  // Debug: motion routing diagnostics
  _Atomic uint64_t dbg_move_calls;
  _Atomic uint64_t dbg_move_drop_target_mismatch;
  _Atomic uint64_t dbg_move_drop_no_target;
  _Atomic uint64_t dbg_move_target_focus;
  _Atomic uint64_t dbg_move_target_drag;
  _Atomic uint64_t dbg_move_target_pointer;

  // Debug: routing snapshots (manual + key transitions)
  _Atomic uint64_t dbg_routing_snapshots;

#ifndef NDEBUG
  uint64_t dbg_last_motion_log_ns;
#endif
} x11_server_ctx_t;

static x11_server_ctx_t g_srv; // zero-initialized

#ifndef NDEBUG
// Rate-limit to ~10 Hz so logs are readable.
static inline void dbg_motion_log_rl(uint64_t now_ns, const char* fmt, ...) {
  if (now_ns - g_srv.dbg_last_motion_log_ns < 100000000ULL) return; // 100ms
  g_srv.dbg_last_motion_log_ns = now_ns;

  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}
#endif

void x11_debug_set_repaint_storm(int enabled, uint32_t xid)
{
  x11_backend_lock();
  g_srv.debug_storm = enabled ? 1 : 0;
  g_srv.debug_storm_xid = xid;

  // Kick the first repaint immediately if possible.
  // Decide under lock, apply outside lock.
  uint32_t kick_xid = 0;
  if (g_srv.debug_storm && g_srv.debug_storm_xid != 0) {
    // Backend will ignore mark_damage if missing/closing.
    kick_xid = g_srv.debug_storm_xid;
  }
  
#ifndef NDEBUG
  x11_backend_debug_check_all_invariants_locked();
#endif

  x11_backend_unlock();
  
  if (kick_xid != 0) {
    x11_backend_mark_damage(kick_xid);
  }

#ifndef NDEBUG
  x11_debug_dump_routing_snapshot(enabled ? "debug_set_repaint_storm(on)" : "debug_set_repaint_storm(off)");
#endif
  x11_server_wakeup();
}

void x11_debug_destroy_during_next_repaint(int enabled, uint32_t xid)
{
  x11_backend_lock();
  g_srv.debug_destroy_during_repaint = enabled ? 1 : 0;
  g_srv.debug_destroy_during_repaint_xid = xid;
  x11_backend_unlock();
}


static void x11_debug_print_build_mode(void) {
#ifdef NDEBUG
  fprintf(stderr, "[SwiftX11] BUILD: RELEASE (NDEBUG defined)\n");
#else
  fprintf(stderr, "[SwiftX11] BUILD: DEBUG (NDEBUG NOT defined)\n");
#endif
}


static inline uint64_t x11_now_ns(void)
{
  static mach_timebase_info_data_t s_tb = {0,0};
  if (s_tb.denom == 0) {
    (void)mach_timebase_info(&s_tb);
  }
  const uint64_t t = mach_continuous_time(); // monotonic on macOS
                                             // Convert to ns: t * numer / denom
  __uint128_t ns = (__uint128_t)t * (__uint128_t)s_tb.numer;
  ns /= (__uint128_t)s_tb.denom;
  return (uint64_t)ns;
}


// ---- Helper for async destroy (safe from repaint context)
struct x11_destroy_arg { uint32_t xid; };

static void x11_emit_window_create(uint32_t xid, const char* title, int32_t w, int32_t h)
{
  // ensure nonzero
  int32_t ww = (w < 1) ? 1 : w;
  int32_t hh = (h < 1) ? 1 : h;

  // Ensure backend state exists, then set size + mark damage.
  x11_backend_alloc_slot(xid);
  x11_backend_window_set_size(xid, ww, hh);
  x11_backend_mark_damage(xid);
  x11_backend_lock();
  x11_backend_window_set_mapped_locked(xid, 0);
  x11_backend_unlock();
  
  // Ask Swift to create the NSWindow
  x11_window_created_cb on_create = NULL;
  x11_backend_lock();
  on_create = g_srv.on_create;
  x11_backend_unlock();

  if (on_create) {
    on_create(xid, title, (int)ww, (int)hh);
  }
  
  // Enqueue event as backend truth
  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_CREATE;
  ev.size = sizeof(ev.u.win_create);
  ev.u.win_create.width_px = ww;
  ev.u.win_create.height_px = hh;
  (void)x11_events_push(&ev);

  x11_server_wakeup();
}


static void x11_emit_window_destroy(uint32_t xid)
{
  x11_window_closed_cb on_close = NULL;
  uint32_t *old_fb = NULL;
  void *retired = NULL;
  
  int was_mapped = 0;
  
  x11_backend_lock();

  // Idempotent destroy: if it's already gone, do nothing.
  if (!x11_backend_window_exists_locked(xid)) {
    atomic_fetch_add_explicit(&g_srv.dbg_destroy_noop, 1, memory_order_relaxed);
  #ifndef NDEBUG
    fprintf(stderr, "[SwiftX11] destroy noop xid=0x%X (already gone)\n", xid);
  #endif
    x11_backend_unlock();
    return;
  }
    
  // If we’re doing the repaint-storm test, stop generating new damage now.
  g_srv.debug_storm = 0;
  g_srv.debug_storm_xid = 0;

  // Snapshot whether the window is mapped so we can emit UNMAP before DESTROY.
  was_mapped = x11_backend_window_is_mapped_locked(xid);
  
  // Prevent *new* repaints from starting on this window.
  (void)x11_backend_window_begin_close_locked(xid);
  
  // Force mapped=0 as part of teardown semantics.
  if (was_mapped) {
    (void)x11_backend_window_set_mapped_locked(xid, 0);
  }

  // Clear routing ownership while we still know xid.
  if (g_srv.pointer_xid == xid) g_srv.pointer_xid = 0;
  if (g_srv.focus_xid == xid)   g_srv.focus_xid = 0;
  if (g_srv.drag_xid == xid)    g_srv.drag_xid = 0;

  // Snapshot callback.
  on_close = g_srv.on_close;

  // Wait for any in-flight repaints to complete.
  uint32_t inflight = x11_backend_repaint_inflight_locked(xid);

  if (inflight != 0) {
    atomic_fetch_add_explicit(&g_srv.debug_destroy_waits, 1, memory_order_relaxed);
    #ifndef NDEBUG
    fprintf(stderr, "[SwiftX11] destroy xid=0x%X waiting inflight=%u\n", xid, inflight);
    #endif
  }  
  (void)x11_backend_wait_inflight_zero_locked(xid);
  
  // Now inflight is 0; detach fb + clear slot via backend (under the same lock).
  (void)x11_backend_window_destroy_locked(xid, &old_fb, &retired);
  
#ifndef NDEBUG
  x11_backend_debug_check_all_invariants_locked();
#endif

  x11_backend_unlock();

#ifndef NDEBUG
  x11_debug_dump_routing_snapshot("emit_window_destroy: after slot cleared");
#endif

  // Free outside lock.
  if (old_fb) free(old_fb);
  if (retired) x11_backend_free_retired(retired);
  
  // If the window was mapped, emit UNMAP before DESTROY (X11-ish ordering).
  uint64_t timestamp = x11_now_ns();
  if (was_mapped) {
    x11_event_t un = (x11_event_t){0};
    un.timestamp_ns = timestamp;
    un.xid = xid;
    un.type = X11_EV_WINDOW_UNMAP;
    un.size = sizeof(un.u.win_unmap);
    (void)x11_events_push(&un);
  }

  // Enqueue DESTROY as backend truth.
  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = timestamp;
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_DESTROY;
  ev.size = sizeof(ev.u.win_destroy);
  (void)x11_events_push(&ev);

  // Ask Swift to actually close the NSWindow (NO LOCK held).
  if (on_close) {
    on_close(xid);
  }

  x11_server_wakeup();
}

static void* x11_destroy_thread_main(void* p)
{
    struct x11_destroy_arg* a = (struct x11_destroy_arg*)p;
    uint32_t xid = a ? a->xid : 0;
    if (a) free(a);
    if (xid) x11_emit_window_destroy(xid);
    return NULL;
}

// Public async destroy helper: safe to call from contexts that must not block
// (e.g., during a repaint while repaint_inflight is held).
static void x11_emit_window_destroy_async(uint32_t xid)
{
  if (xid == 0) return;

  pthread_t t;
  struct x11_destroy_arg* arg = (struct x11_destroy_arg*)malloc(sizeof(*arg));
  if (!arg) return;
  arg->xid = xid;

  const int rc = pthread_create(&t, NULL, x11_destroy_thread_main, arg);
  if (rc == 0) {
    pthread_detach(t);
  } else {
    free(arg);
  }
}

static int x11_srv_init_runloop_cv_locked(void)
{
  if (g_srv.runloop_cv_inited) return 1;

  pthread_condattr_t attr;
  int rc = pthread_condattr_init(&attr);
  if (rc != 0) return 0;

  rc = pthread_cond_init(&g_srv.cv, &attr);
  pthread_condattr_destroy(&attr);
  if (rc != 0) return 0;

  g_srv.runloop_cv_inited = 1;
  return 1;
}

static void x11_srv_destroy_runloop_cv_locked(void)
{
  if (!g_srv.runloop_cv_inited) return;
  pthread_cond_destroy(&g_srv.cv);
  g_srv.runloop_cv_inited = 0;
}


// ---- Debug helpers
void x11_debug_dump_window_table(void)
{
    x11_backend_lock();
  
    fprintf(stderr, "\n[SwiftX11] Window table dump:\n");
    fprintf(stderr, "  pointer_xid=0x%X focus_xid=0x%X drag_xid=0x%X buttons=0x%X storm=%d storm_xid=0x%X\n",
            g_srv.pointer_xid, g_srv.focus_xid, g_srv.drag_xid, g_srv.buttons, g_srv.debug_storm, g_srv.debug_storm_xid);

    #ifndef NDEBUG
    x11_backend_debug_win_t wins[X11_MAX_WINDOWS];
    int wn = x11_backend_debug_snapshot_windows_locked(wins, X11_MAX_WINDOWS);
    for (int i = 0; i < wn; i++) {
        fprintf(stderr,
                "  xid=0x%X size=%dx%d damaged=%u closing=%u inflight=%u\n debug_destroy_waits=%u\n",
                wins[i].xid,
                (int)wins[i].w_px,
                (int)wins[i].h_px,
                (unsigned)wins[i].damaged,
                (unsigned)wins[i].closing,
                (unsigned)wins[i].inflight,
                (unsigned)wins[i].debug_destroy_waits);
    }
#else
    // In release builds, keep output minimal.
    (void)0;
#endif

    fprintf(stderr, "[SwiftX11] End window table dump.\n\n");

    x11_backend_unlock();
}


int x11_debug_get_window_alive(uint32_t xid)
{
  x11_backend_lock();
  int exists = x11_backend_window_exists_locked(xid);
  x11_backend_unlock();
  return exists;
}


int x11_debug_get_window_size(uint32_t xid, int32_t* out_w_px, int32_t* out_h_px)
{
    if (!out_w_px || !out_h_px) return 0;

    x11_backend_lock();
    #ifndef NDEBUG
    // Use backend snapshot so shim doesn't touch g_windows.
    x11_backend_debug_win_t wins[X11_MAX_WINDOWS];
    int wn = x11_backend_debug_snapshot_windows_locked(wins, X11_MAX_WINDOWS);
    int ok = 0;
    for (int i = 0; i < wn; i++) {
        if (wins[i].xid == xid) {
            *out_w_px = wins[i].w_px;
            *out_h_px = wins[i].h_px;
            ok = 1;
            break;
        }
    }
    x11_backend_unlock();
    return ok;
#else
    x11_backend_unlock();
    return 0;
#endif
}

int x11_debug_get_snapshot(x11_debug_snapshot_t* out_snapshot)
{
    if (!out_snapshot) return 0;
    memset(out_snapshot, 0, sizeof(*out_snapshot));

    // Queue stats (atomic inside x11_events.c)
    out_snapshot->q_count = x11_events_count();
    out_snapshot->q_motion_overwrites = x11_debug_motion_overwrites();
    out_snapshot->q_push_drops = x11_debug_push_drops();

    // Backend + routing state must be read under g_mu
    x11_backend_lock();
  
    out_snapshot->pointer_xid = g_srv.pointer_xid;
    out_snapshot->focus_xid   = g_srv.focus_xid;
    out_snapshot->drag_xid    = g_srv.drag_xid;
    out_snapshot->buttons     = g_srv.buttons;
      atomic_load_explicit(&g_srv.debug_destroy_waits, memory_order_relaxed);
  
    #ifndef NDEBUG
    x11_backend_debug_win_t wins[X11_MAX_WINDOWS];
    int wn = x11_backend_debug_snapshot_windows_locked(wins, X11_MAX_WINDOWS);

    uint32_t n = 0;
    for (int i = 0; i < wn && n < X11_DEBUG_MAX_WINDOWS; i++) {
        out_snapshot->windows[n].xid     = wins[i].xid;
        out_snapshot->windows[n].w_px                = wins[i].w_px;
        out_snapshot->windows[n].h_px                = wins[i].h_px;
        out_snapshot->windows[n].alive               = wins[i].alive;
        out_snapshot->windows[n].damaged             = wins[i].damaged;
        n++;
    }
    out_snapshot->window_count = n;
#else
    out_snapshot->window_count = 0;
#endif

    x11_backend_unlock();
    return 1;
}

void x11_register_callbacks(
                            x11_window_created_cb on_create,
                            x11_window_closed_cb on_close)
{
  x11_backend_lock();
  g_srv.on_create = on_create;
  g_srv.on_close  = on_close;
  x11_backend_unlock();
}

bool x11_start_server(int32_t display)
{
  (void)display;

  // mouse and keyboard event handling
  x11_events_init();

  // Ensure backend process-lifetime primitives exist
  x11_backend_init();

  // Reset window table for a clean start
  x11_backend_clear_windows();
  
#if 0
  x11_debug_dump_window_table();
#endif

  // Start repaint runloop
  x11_server_runloop_start();

  return true;
}

void x11_stop_server(void)
{
  // Snapshot live windows via backend (avoid iterating while mutating)
  uint32_t live[X11_MAX_WINDOWS];
  int n = x11_backend_snapshot_live_xids(live, X11_MAX_WINDOWS);

  // Close all live windows (idempotent destroy makes this safe)
  for (int i = 0; i < n; i++) {
    x11_emit_window_destroy(live[i]);
  }

#if 0
  x11_debug_dump_window_table();
#endif

  x11_server_runloop_stop();

  // Clear backend window table so second start is always clean
  x11_backend_clear_windows();
  
  x11_events_shutdown();
}

void x11_register_frame_presenter(x11_present_frame_cb on_present)
{
  x11_backend_lock();
  g_srv.present = on_present;
  x11_backend_unlock();

  // When the presenter becomes available, mark all live windows damaged
  // so their first frame is guaranteed to be produced.
  if (on_present) {
    x11_backend_mark_all_damage();
  }

  // Wake the runloop so we repaint immediately (rather than waiting for timeout).
  x11_server_wakeup();
}

void x11_request_repaint(uint32_t xwin_id, int32_t width_px, int32_t height_px)
{
  if (width_px <= 0 || height_px <= 0) return;

  x11_present_frame_cb presenter = NULL;
  uint32_t *fb = NULL;

  bool inflight_taken = false;

  // One-shot destroy test
  int do_destroy = 0;
  uint32_t dxid = 0;

  // 1) Snapshot presenter + check window + take inflight + snapshot destroy-test state
  x11_backend_lock();
  presenter = g_srv.present;

  if (!presenter) {
    x11_backend_unlock();
    return;
  }

  if (!x11_backend_repaint_begin_locked(xwin_id)) {
    // begin_locked is the single source of truth:
    // it fails if missing / not alive / closing.
    x11_backend_unlock();
    return;
  }
  inflight_taken = true;

  if (g_srv.debug_destroy_during_repaint &&
      g_srv.debug_destroy_during_repaint_xid == xwin_id)
  {
    do_destroy = 1;
    dxid = g_srv.debug_destroy_during_repaint_xid;
    g_srv.debug_destroy_during_repaint = 0; // one-shot
  }

#ifndef NDEBUG
  x11_backend_debug_check_invariants_for_xid_locked(xwin_id);
#endif

  x11_backend_unlock();

  // debugging -- slow down the repaint (no lock held)
  if (g_srv.debug_storm) { usleep(20 * 1000); }  // 20ms

  if (do_destroy) {
    // IMPORTANT: never call synchronous destroy from inside repaint
    x11_post_window_destroy_async(dxid);
  }

  // 2) Ensure backing store exists (backend locks internally)
  const size_t need_pixels = (size_t)width_px * (size_t)height_px;
  const int bpr = width_px * 4;

  if (!x11_backend_ensure_fb(xwin_id, need_pixels, &fb) || !fb) {
    goto done;
  }

  // 3) Render into fb (no lock held)
  // temporary pattern for now based on xwin_id
  // Derive a stable per-window seed from xid
  uint32_t seed = xwin_id * 2654435761u; // Knuth multiplicative hash

  // Simple per-window base color
  uint8_t base_r = (seed >>  0) & 0xFF;
  uint8_t base_g = (seed >>  8) & 0xFF;
  uint8_t base_b = (seed >> 16) & 0xFF;
  #ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] REPAINT xid=0x%X seed=0x%08X base=%u,%u,%u\n",
          xwin_id, seed, base_r, base_g, base_b);
  #endif
  
  for (int y = 0; y < height_px; y++) {
    for (int x = 0; x < width_px; x++) {
      uint8_t a = 0xFF;

      uint8_t r = (uint8_t)(base_r ^ (x & 0xFF));
      uint8_t g = (uint8_t)(base_g ^ (y & 0xFF));
      uint8_t b = (uint8_t)(base_b ^ ((x + y) & 0xFF));
      
      if (((x / (16 + (seed & 31))) & 1) ^ ((y / (16 + ((seed >> 5) & 31))) & 1))  {
        r = (uint8_t)(255 - r);
        g = (uint8_t)(255 - g);
        b = (uint8_t)(255 - b);
      }
      
      fb[(size_t)y * (size_t)width_px + (size_t)x] =
        (uint32_t)(b | (g << 8) | (r << 16) | (a << 24));
    }
  }

  // 4) Present (no lock held)
  presenter(xwin_id, fb, width_px, height_px, bpr);

done:
  if (inflight_taken) {
    void *retired = NULL;

    x11_backend_lock();
    x11_backend_repaint_end_locked(xwin_id, &retired);

    #ifndef NDEBUG
    x11_backend_debug_check_invariants_for_xid_locked(xwin_id);
    #endif

    x11_backend_unlock();
    
    if (retired) x11_backend_free_retired(retired);
  }
}



void x11_post_pointer_event(uint32_t xid, x11_ptr_event_type type,
                            int32_t x_px, int32_t y_px,
                            uint32_t buttons, uint32_t modifiers)
{
    // Motion routing (extra-correct for macOS):
    // - If a drag-grab is active, route motion to the grab window.
    // Motion routing (deterministic):
    // - If a drag-grab is active, route motion to the grab window.
    // - Otherwise, route motion to the current pointer owner (enter/leave ownership).
    // - If no pointer owner exists, fall back to the focused window (if any).
    //
    // NOTE: This is intentionally pointer-owner-first (more X11-ish). If you later want
    // "key window receives mouseMoved even when pointer is outside", swap focus/pointer.    
  if (type == X11_PTR_MOVE) {
        atomic_fetch_add_explicit(&g_srv.dbg_move_calls, 1, memory_order_relaxed);
        uint32_t drag = 0;
        uint32_t focus = 0;
        uint32_t pointer = 0;

        x11_backend_lock();
        drag = g_srv.drag_xid;
        focus = g_srv.focus_xid;
        pointer = g_srv.pointer_xid;
        x11_backend_unlock();

      uint32_t target = 0;
      if (drag != 0) {
        target = drag;
        atomic_fetch_add_explicit(&g_srv.dbg_move_target_drag, 1, memory_order_relaxed);
      } else if (pointer != 0) {
        target = pointer;
        atomic_fetch_add_explicit(&g_srv.dbg_move_target_pointer, 1, memory_order_relaxed);
      } else {
        target = focus;
        atomic_fetch_add_explicit(&g_srv.dbg_move_target_focus, 1, memory_order_relaxed);
      }

      if (target == 0) {
        atomic_fetch_add_explicit(&g_srv.dbg_move_drop_no_target, 1, memory_order_relaxed);
      #ifndef NDEBUG
        dbg_motion_log_rl(x11_now_ns(),
          "[SwiftX11] MOVE drop: no target (from xid=0x%X drag=0x%X focus=0x%X ptr=0x%X)\n",
          xid, drag, focus, pointer);
      #endif
        return;
      }

      // Deterministic routing: rewrite to the computed target instead of dropping.
      if (target != xid) {
        atomic_fetch_add_explicit(&g_srv.dbg_move_drop_target_mismatch, 1, memory_order_relaxed);
      #ifndef NDEBUG
        dbg_motion_log_rl(x11_now_ns(),
          "[SwiftX11] MOVE reroute: from xid=0x%X -> target=0x%X (drag=0x%X focus=0x%X ptr=0x%X)\n",
          xid, target, drag, focus, pointer);
      #endif
        xid = target;
      }

      #ifndef NDEBUG
      dbg_motion_log_rl(x11_now_ns(),
        "[SwiftX11] MOVE ok: xid=0x%X (drag=0x%X focus=0x%X ptr=0x%X)\n",
        xid, drag, focus, pointer);
      #endif
    }

    // Keep canonical backend button/drag state correct even for the legacy API.
    // NOTE: Some callers historically passed 0 for motion. Therefore:
    //   - For motion: NEVER trust `buttons`; read g_srv.buttons instead.
    //   - For down/up: treat `buttons` as the AFTER-state mask and commit it to g_srv.buttons.
    // Drag ownership is derived from the 0->nonzero and nonzero->0 transitions.
    uint32_t buttons_snapshot = 0;

    if (type == X11_PTR_DOWN || type == X11_PTR_UP) {
        x11_backend_lock();
      
        // A click implies pointer ownership.
        if (type == X11_PTR_DOWN) {
            g_srv.pointer_xid = xid;
        }

        const uint32_t old_buttons = g_srv.buttons;
        const uint32_t new_buttons = buttons; // legacy API supplies AFTER-state mask
        g_srv.buttons = new_buttons;

        // Drag grab behavior: first transition to nonzero grabs; transition to zero releases.
        if (old_buttons == 0 && new_buttons != 0) {
            g_srv.drag_xid = xid;
        } else if (old_buttons != 0 && new_buttons == 0) {
            g_srv.drag_xid = 0;
        }

        buttons_snapshot = g_srv.buttons;
        x11_backend_unlock();
    } else if (type == X11_PTR_MOVE) {
        x11_backend_lock();
        buttons_snapshot = g_srv.buttons;
        x11_backend_unlock();
    }

    x11_event_t ev = (x11_event_t){0};
    ev.timestamp_ns = x11_now_ns();
    ev.xid = xid;

    ev.type = (type == X11_PTR_MOVE) ? X11_EV_POINTER_MOTION :
              (type == X11_PTR_DOWN || type == X11_PTR_UP) ? X11_EV_POINTER_BUTTON :
              X11_EV_NONE;

    if (ev.type == X11_EV_POINTER_MOTION) {
        ev.size = sizeof(ev.u.motion);
        ev.u.motion.x_px = x_px;
        ev.u.motion.y_px = y_px;
        ev.u.motion.buttons = buttons_snapshot;
        ev.u.motion.modifiers = modifiers;
        (void)x11_events_push(&ev);
        return;
    }

    if (ev.type == X11_EV_POINTER_BUTTON) {
        ev.size = sizeof(ev.u.button);
        ev.u.button.x_px = x_px;
        ev.u.button.y_px = y_px;

        // Legacy API can't identify which button changed.
        ev.u.button.button = 0;
        ev.u.button.is_press = (type == X11_PTR_DOWN) ? 1 : 0;

        // AFTER-state mask
        ev.u.button.buttons = buttons_snapshot;
        ev.u.button.modifiers = modifiers;
        (void)x11_events_push(&ev);
        return;
    }
}

void x11_post_key_event(uint32_t xid, bool is_down,
                        uint32_t keycode, uint32_t modifiers,
                        const char* utf8_text)
{
  // Route xid==0 to the currently focused window
  uint32_t target = xid;
  if (target == 0) {
    x11_backend_lock();
    target = g_srv.focus_xid;
    x11_backend_unlock();
  }

  // If nothing focused, drop the key
  if (target == 0) return;

  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = target;
  ev.type = X11_EV_KEY;
  ev.size = sizeof(ev.u.key);
  ev.u.key.keycode = keycode;
  ev.u.key.is_press = is_down ? 1 : 0;
  ev.u.key.modifiers = modifiers;
  
  if (is_down && utf8_text) {
    size_t n = strnlen(utf8_text, X11_TEXT_MAX);
    ev.u.key.text_len = (uint8_t)n;
    memcpy(ev.u.key.text_utf8, utf8_text, n);
  } else {
    ev.u.key.text_len = 0;
  }
  
  (void)x11_events_push(&ev);
}

void x11_post_pointer_button(uint32_t xid,
                             bool is_press,
                             uint8_t button,
                             int32_t x_px,
                             int32_t y_px,
                             uint32_t buttons,
                             uint32_t modifiers)
{
  x11_backend_lock();
  
  // Maintain canonical button state here instead of trusting the caller.
  // Some call sites send a release while the local mask still includes the bit,
  // so we force the bit on press and clear it on release.
  const uint32_t old_buttons = g_srv.buttons;

  uint32_t mask = buttons;
  if (button >= 1 && button <= 31) {
      const uint32_t bit = (1u << (uint32_t)(button - 1u));
      if (is_press) {
          mask |= bit;
      } else {
          mask &= ~bit;
      }
  }

  // EXTRA-CORRECT: a click implies pointer ownership for routing.
  if (is_press) {
      g_srv.pointer_xid = xid;
  }

  // Canonical after-state button mask.
  g_srv.buttons = mask;

  // Drag ownership is derived from transitions of the canonical mask.
  if (old_buttons == 0 && g_srv.buttons != 0) {
      g_srv.drag_xid = xid;
  } else if (old_buttons != 0 && g_srv.buttons == 0) {
      g_srv.drag_xid = 0;
  }

  const uint32_t buttons_snapshot = g_srv.buttons;

  x11_backend_unlock();

#ifndef NDEBUG
  x11_debug_dump_routing_snapshot(is_press ? "pointer_button(down)" : "pointer_button(up)");
#endif
  
  x11_event_t ev = {0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid  = xid;
  ev.type = X11_EV_POINTER_BUTTON;
  ev.size = sizeof(ev.u.button);
  
  ev.u.button.x_px = x_px;
  ev.u.button.y_px = y_px;
  ev.u.button.button = button;
  ev.u.button.is_press = is_press ? 1 : 0;
  ev.u.button.buttons = buttons_snapshot;
  ev.u.button.modifiers = modifiers;
  
  (void)x11_events_push(&ev);
}

void x11_post_scroll_ticks(uint32_t xid,
                           x11_scroll_axis_t axis,
                           int16_t ticks,
                           int32_t x_px,
                           int32_t y_px,
                           uint32_t buttons,
                           uint32_t modifiers)
{
  // Deterministic scroll routing (match MOVE routing): drag > focus > pointer.
  // Also snapshot canonical button state instead of trusting the caller.
  uint32_t drag = 0;
  uint32_t focus = 0;
  uint32_t pointer = 0;
  uint32_t buttons_snapshot = 0;

  x11_backend_lock();
  drag = g_srv.drag_xid;
  focus = g_srv.focus_xid;
  pointer = g_srv.pointer_xid;
  buttons_snapshot = g_srv.buttons;
  x11_backend_unlock();

  uint32_t target = 0;
  if (drag != 0) {
    target = drag;
  } else if (pointer != 0) {
    target = pointer;
  } else {
    target = focus;
  }

  if (target == 0) {
    return;
  }

  if (target != xid) {
    xid = target;
  }
  
  
  x11_event_t ev = {0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid  = xid;
  ev.type = X11_EV_SCROLL;
  ev.size = sizeof(ev.u.scroll);
  
  ev.u.scroll.x_px = x_px;
  ev.u.scroll.y_px = y_px;
  ev.u.scroll.axis = (uint8_t)axis; // store as byte in the struct
  ev.u.scroll.ticks = ticks;
  ev.u.scroll.buttons = buttons_snapshot;
  ev.u.scroll.modifiers = modifiers;
  
  (void)x11_events_push(&ev);
}

void x11_post_focus_event(uint32_t xid, bool focused)
{
  uint32_t prev_focus = 0;
  uint32_t prev_ptr = 0;
  uint32_t prev_drag = 0;

  x11_backend_lock();
  prev_focus = g_srv.focus_xid;
  prev_ptr   = g_srv.pointer_xid;
  prev_drag  = g_srv.drag_xid;

  if (focused) {
    g_srv.focus_xid = xid;
    g_srv.pointer_xid = xid;
  } else {
    if (g_srv.focus_xid == xid) g_srv.focus_xid = 0;
    if (g_srv.pointer_xid == xid && g_srv.drag_xid == 0) g_srv.pointer_xid = 0;
  }

  #ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] FOCUS xid=0x%X focused=%d (prev_focus=0x%X) ptr=0x%X->0x%X drag=0x%X\n",
          xid, focused ? 1 : 0, prev_focus, prev_ptr, g_srv.pointer_xid, prev_drag);
  #endif

  x11_backend_unlock();
  
#ifndef NDEBUG
  x11_debug_dump_routing_snapshot(focused ? "focus(true)" : "focus(false)");
#endif
  
  x11_event_t ev = {0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_FOCUS;
  ev.size = sizeof(ev.u.focus);
  ev.u.focus.focused = focused ? 1 : 0;
  (void)x11_events_push(&ev);
}

void x11_post_pointer_enter(uint32_t xid,
                            int32_t x_px,
                            int32_t y_px,
                            uint32_t modifiers)
{
  uint32_t prev = 0;
  uint32_t drag = 0;
  x11_backend_lock();
  prev = g_srv.pointer_xid;
  drag = g_srv.drag_xid;
  if (drag == 0) {
    g_srv.pointer_xid = xid;
  }
  #ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] ENTER xid=0x%X (ptr 0x%X->0x%X) drag=0x%X\n",
          xid, prev, g_srv.pointer_xid, drag);
  #endif
  x11_backend_unlock();
  
#ifndef NDEBUG
  x11_debug_dump_routing_snapshot("pointer_enter");
#endif
  
  x11_event_t ev = {0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_POINTER_ENTER;
  ev.size = sizeof(ev.u.crossing);
  
  ev.u.crossing.x_px = x_px;
  ev.u.crossing.y_px = y_px;
  ev.u.crossing.modifiers = modifiers;
  
  (void)x11_events_push(&ev);
}

void x11_post_pointer_leave(uint32_t xid,
                            int32_t x_px,
                            int32_t y_px,
                            uint32_t modifiers)
{
  uint32_t prev = 0;
  uint32_t drag = 0;
  x11_backend_lock();
  prev = g_srv.pointer_xid;
  drag = g_srv.drag_xid;
  if (drag == 0 && g_srv.pointer_xid == xid) {
    g_srv.pointer_xid = 0;
  }
  #ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] LEAVE xid=0x%X (ptr 0x%X->0x%X) drag=0x%X\n",
          xid, prev, g_srv.pointer_xid, drag);
  #endif
  x11_backend_unlock();
  
#ifndef NDEBUG
  x11_debug_dump_routing_snapshot("pointer_leave");
#endif
  
  x11_event_t ev = {0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_POINTER_LEAVE;
  ev.size = sizeof(ev.u.crossing);
  
  ev.u.crossing.x_px = x_px;
  ev.u.crossing.y_px = y_px;
  ev.u.crossing.modifiers = modifiers;
  
  (void)x11_events_push(&ev);
}

bool x11_debug_pop_event(x11_event_t* out_ev)
{
  return x11_events_pop(out_ev);
}

void x11_post_window_raise(uint32_t xid)
{
    x11_event_t ev = {0};
    ev.timestamp_ns = x11_now_ns();
    ev.xid = xid;
    ev.type = X11_EV_WINDOW_RAISE;
    ev.size = sizeof(ev.u.raise);
    (void)x11_events_push(&ev);
}


void x11_post_window_destroy(uint32_t xid)
{
#ifndef NDEBUG
  // In debug, synchronous destroy. Safe from normal UI/control paths.
    x11_emit_window_destroy(xid);
#else
    // In release, always use async to avoid deadlock hazards.
    x11_emit_window_destroy_async(xid);
#endif
}


void x11_post_window_destroy_async(uint32_t xid)
{
    // Asynchronous destroy: safe to call from contexts that must not block.
    x11_emit_window_destroy_async(xid);
}


void x11_post_window_map(uint32_t xid)
{
  if (xid == 0) return;

  x11_backend_lock();
  int exists  = x11_backend_window_exists_locked(xid);
  int closing = x11_backend_window_is_closing_locked(xid);
  int mapped  = exists ? x11_backend_window_is_mapped_locked(xid) : 0;

  if (!exists || closing) {
    x11_backend_unlock();
    return;
  }

  // Idempotent state change (only flip if needed)
  if (!mapped) {
    (void)x11_backend_window_set_mapped_locked(xid, 1);
    x11_backend_mark_damage_locked(xid);
  }
  x11_backend_unlock();

  // ALWAYS enqueue MAP so Swift can perform the side-effect (show window).
  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_MAP;
  ev.size = sizeof(ev.u.win_map);
  (void)x11_events_push(&ev);

  x11_server_wakeup();
}


void x11_post_window_unmap(uint32_t xid)
{
  if (xid == 0) return;

  x11_backend_lock();
  int exists = x11_backend_window_exists_locked(xid);
  int mapped = exists ? x11_backend_window_is_mapped_locked(xid) : 0;
  if (!exists) { x11_backend_unlock(); return; }

  if (mapped) {
    (void)x11_backend_window_set_mapped_locked(xid, 0);
  }
  x11_backend_unlock();

  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_UNMAP;
  ev.size = sizeof(ev.u.win_unmap);
  (void)x11_events_push(&ev);
}


void x11_post_window_resize(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (xid == 0) return;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  // Gate on existence (and optionally "not closing") to avoid stale events.
  x11_backend_lock();
  const int exists = x11_backend_window_is_alive_locked(xid);
  const int closing = x11_backend_window_is_closing_locked(xid);
  x11_backend_unlock();

  if (!exists || closing) {
    return;
  }

  // Update backend truth first.
  x11_backend_window_set_size(xid, w_px, h_px);

  // Resizing implies we need a repaint.
  x11_backend_mark_damage(xid);

  // Emit event for Swift-side observers/logging.
  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_RESIZE;
  ev.size = sizeof(ev.u.win_resize);
  ev.u.win_resize.width_px  = w_px;
  ev.u.win_resize.height_px = h_px;
  (void)x11_events_push(&ev);

  // Wake repaint loop so the resize shows immediately.
  x11_server_wakeup();
}

void x11_set_window_size(uint32_t xid, int32_t width_px, int32_t height_px)
{
  if (width_px < 1) width_px = 1;
  if (height_px < 1) height_px = 1;

  // Only window-create should allocate. Resize should be a no-op if missing.
  x11_backend_window_set_size(xid, width_px, height_px);
  x11_backend_mark_damage(xid);
  x11_server_wakeup();
}

void x11_mark_damage(uint32_t xid)
{
  x11_backend_mark_damage(xid);
  x11_server_wakeup();
}

void x11_server_wakeup(void)
{
  x11_backend_lock();
  if (g_srv.runloop_running && g_srv.runloop_cv_inited) {
    x11_backend_cond_signal(&g_srv.cv);
  }
  x11_backend_unlock();
}

// process one “tick”: snapshot damaged windows and repaint them.
void x11_server_step(void)
{
  // 0) Apply queued client requests first (server thread truth)
  x11_requests_drain_on_server_thread();   // new

  // 1) Then snapshot damaged windows and repaint them.
  uint32_t xids[X11_MAX_WINDOWS];
  int32_t  ws[X11_MAX_WINDOWS];
  int32_t  hs[X11_MAX_WINDOWS];
  int n = 0;

  // Ask backend for the list of damaged windows (also clears damage internally)
  n = x11_backend_take_damaged_snapshot(xids, ws, hs, X11_MAX_WINDOWS);

  // Debug storm: continuously re-damage one window so the runloop repaints every tick.
  if (g_srv.debug_storm && g_srv.debug_storm_xid != 0) {
    x11_backend_mark_damage(g_srv.debug_storm_xid);
  }

  #ifndef NDEBUG
  // Backend owns invariants; this check still validates the shared window table.
  x11_backend_lock();
  x11_backend_debug_check_all_invariants_locked();
  x11_backend_unlock();
  #endif
  
  // Perform repaints outside lock
  for (int i = 0; i < n; i++) {
    x11_request_repaint(xids[i], ws[i], hs[i]);
  }
}

static void* runloop_main(void* _)
{
  (void)_;
    x11_backend_lock();
  
  while (!g_srv.runloop_stop) {
#ifndef NDEBUG
    assert(g_srv.runloop_cv_inited);
#else
    if (!g_srv.runloop_cv_inited) break;
#endif
    
    // Sleep until woken (or timeout for safety)
    struct timespec ts;
    // macOS pthread condition variables use CLOCK_REALTIME for absolute timeouts.
    // (pthread_condattr_setclock is not available on macOS.)
    clock_gettime(CLOCK_REALTIME, &ts);
    
    // ~16ms timeout (60Hz) so we still repaint if a wake is missed
    ts.tv_nsec += 16 * 1000 * 1000;
    while (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    // since start guarantees cv init
    x11_backend_cond_timedwait(&g_srv.cv, &ts);
    
    x11_backend_unlock();
    x11_server_step();
    x11_backend_lock();
  }

    x11_backend_unlock();
    return NULL;
}

void x11_server_runloop_start(void)
{
  
  // temporary
  x11_debug_print_build_mode();
  
  x11_backend_lock();
  
  if (!x11_srv_init_runloop_cv_locked()) {
#ifndef NDEBUG
    fprintf(stderr, "[SwiftX11] runloop_start: failed to init cv\n");
#endif
    x11_backend_unlock();
    return;
  }


  if (g_srv.runloop_running) {
    x11_backend_unlock();
    return;
  }
  
  g_srv.runloop_stop = 0;
  g_srv.runloop_running = 1;
  
  x11_backend_unlock();

  pthread_create(&g_srv.thread, NULL, runloop_main, NULL);
}

void x11_server_runloop_stop(void)
{
  x11_backend_lock();
  if (!g_srv.runloop_running) {
    x11_backend_unlock();
    return;
  }
  
  g_srv.runloop_stop = 1;
  if (g_srv.runloop_cv_inited) {
    x11_backend_cond_signal(&g_srv.cv);
  }
  x11_backend_unlock();
  
  pthread_join(g_srv.thread, NULL);
  
  x11_backend_lock();
  g_srv.runloop_running = 0;
  x11_srv_destroy_runloop_cv_locked();   // if you have it
  x11_backend_unlock();
}


void x11_debug_dump_routing_snapshot(const char* reason)
{
#ifndef NDEBUG
  const char* why = (reason && reason[0]) ? reason : "(no reason)";

  // Motion diagnostics (atomics)
  uint64_t move_calls    = atomic_load_explicit(&g_srv.dbg_move_calls, memory_order_relaxed);
  uint64_t move_mismatch = atomic_load_explicit(&g_srv.dbg_move_drop_target_mismatch, memory_order_relaxed);
  uint64_t move_no_target= atomic_load_explicit(&g_srv.dbg_move_drop_no_target, memory_order_relaxed);
  uint64_t move_t_focus  = atomic_load_explicit(&g_srv.dbg_move_target_focus, memory_order_relaxed);
  uint64_t move_t_drag   = atomic_load_explicit(&g_srv.dbg_move_target_drag, memory_order_relaxed);
  uint64_t move_t_ptr    = atomic_load_explicit(&g_srv.dbg_move_target_pointer, memory_order_relaxed);

  // Count snapshots
  uint64_t snaps = atomic_fetch_add_explicit(&g_srv.dbg_routing_snapshots, 1, memory_order_relaxed) + 1;

  // Snapshot routing + windows under mutex
  uint32_t pointer = 0, focus = 0, drag = 0, buttons = 0;
  uint64_t destroy_waits = 0;

  struct win_line {
    uint32_t xid;
    int32_t  w;
    int32_t  h;
    uint8_t  damaged;
    uint32_t inflight;
    uint64_t debug_destroy_waits;
  } wins[X11_MAX_WINDOWS];
  int wn = 0;

  x11_backend_lock();
  pointer = g_srv.pointer_xid;
  focus   = g_srv.focus_xid;
  drag    = g_srv.drag_xid;
  buttons = g_srv.buttons;
  destroy_waits = atomic_load_explicit(&g_srv.debug_destroy_waits, memory_order_relaxed);

#ifndef NDEBUG
  x11_backend_debug_win_t bwins[X11_MAX_WINDOWS];
  int bwn = x11_backend_debug_snapshot_windows_locked(bwins, X11_MAX_WINDOWS);
  for (int i = 0; i < bwn && wn < X11_MAX_WINDOWS; i++) {
    wins[wn].xid = bwins[i].xid;
    wins[wn].w   = bwins[i].w_px;
    wins[wn].h   = bwins[i].h_px;
    wins[wn].damaged = bwins[i].damaged;
    wins[wn].inflight = bwins[i].inflight;
    wins[wn].debug_destroy_waits = bwins[i].debug_destroy_waits;
    wn++;
  }
#endif
  x11_backend_unlock();

  fprintf(stderr,
          "[SwiftX11] ROUTING SNAP #%llu reason=%s ptr=0x%X focus=0x%X drag=0x%X buttons=0x%X destroyWaits=%llu\n",
          (unsigned long long)snaps,
          why,
          pointer, focus, drag, buttons,
          (unsigned long long)destroy_waits);

  fprintf(stderr,
          "[SwiftX11]   MOVE calls=%llu mismatch=%llu noTarget=%llu targets: focus=%llu drag=%llu ptr=%llu\n",
          (unsigned long long)move_calls,
          (unsigned long long)move_mismatch,
          (unsigned long long)move_no_target,
          (unsigned long long)move_t_focus,
          (unsigned long long)move_t_drag,
          (unsigned long long)move_t_ptr);

  for (int i = 0; i < wn; i++) {
    fprintf(stderr,
            "[SwiftX11]   WIN xid=0x%X size=%dx%d damaged=%u inflight=%u\n debug_destroy_waits=%u\n",
            wins[i].xid,
            (int)wins[i].w,
            (int)wins[i].h,
            (unsigned)wins[i].damaged,
            (unsigned)wins[i].inflight,
            (unsigned)wins[i].debug_destroy_waits);
  }
#else
  (void)reason;
#endif
}

void x11_debug_reset_routing_counters(void)
{
  atomic_store_explicit(&g_srv.dbg_move_calls, 0, memory_order_relaxed);
  atomic_store_explicit(&g_srv.dbg_move_drop_target_mismatch, 0, memory_order_relaxed);
  atomic_store_explicit(&g_srv.dbg_move_drop_no_target, 0, memory_order_relaxed);
  atomic_store_explicit(&g_srv.dbg_move_target_focus, 0, memory_order_relaxed);
  atomic_store_explicit(&g_srv.dbg_move_target_drag, 0, memory_order_relaxed);
  atomic_store_explicit(&g_srv.dbg_move_target_pointer, 0, memory_order_relaxed);
  atomic_store_explicit(&g_srv.dbg_routing_snapshots, 0, memory_order_relaxed);
}

void x11_debug_torture_once(int iters, int us_between, int allow_destroy)
{
#ifndef NDEBUG
  if (iters < 1) iters = 1;
  if (us_between < 0) us_between = 0;

  uint32_t live[X11_MAX_WINDOWS];
  int n = x11_backend_snapshot_live_xids(live, X11_MAX_WINDOWS);
  if (n <= 0) return;


  fprintf(stderr, "[SwiftX11] torture_once iters=%d us_between=%d\n", iters, us_between);

  // Alternate windows to stress routing and slot lifecycle.
  for (int i = 0; i < iters; i++) {
    uint32_t xid = live[i % n];
    
    // 1) Start a repaint storm (causes constant damage->repaint activity).
    x11_debug_set_repaint_storm(1, xid);

    // 2) Arrange a destroy during the next repaint.
    if (allow_destroy) {
      x11_debug_destroy_during_next_repaint(1, xid);
    }

    // 3) Wake runloop so repaint happens quickly.
    x11_server_wakeup();

    // Let the runloop tick a couple times.
    if (us_between) usleep((useconds_t)us_between);

    // Turn storm off so we don’t just pin the CPU forever.
    x11_debug_set_repaint_storm(0, 0);

    if (us_between) usleep((useconds_t)us_between);
  }

  // Dump final state/counters so you can eyeball that things stabilized.
  x11_debug_dump_routing_snapshot("torture_once: done");
#else
  (void)iters;
  (void)us_between;
#endif
}


uint32_t x11_window_create(const char* title, int32_t w_px, int32_t h_px)
{
  // Generate an xid (simple monotonic allocator)
  uint32_t xid = atomic_fetch_add_explicit(&g_next_xid, 1, memory_order_relaxed);

  // Use existing internal helper so events+damage happen consistently
  const char* t = (title && title[0]) ? title : "SwiftX11 Window";
  x11_emit_window_create(xid, t, w_px, h_px);
  return xid;
}

void x11_window_set_title(uint32_t xid, const char* title_utf8)
{
  if (!title_utf8) title_utf8 = "";

  // Optional: drop if window is gone (keeps queue cleaner).
  x11_backend_lock();
  int exists = x11_backend_window_exists_locked(xid);
  x11_backend_unlock();
  if (!exists) return;

  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_TITLE;
  ev.size = sizeof(ev.u.win_title);

  size_t n = strnlen(title_utf8, X11_TEXT_MAX);
  ev.u.win_title.title_len = (uint8_t)n;
  memcpy(ev.u.win_title.title_utf8, title_utf8, n);

  (void)x11_events_push(&ev);

  // Wake so the UI sees it promptly.
  x11_server_wakeup();
}

// x11_shim.c (non-static wrappers)
void x11_server_emit_window_create(uint32_t xid, const char* title, int32_t w, int32_t h)
{
  x11_emit_window_create(xid, title, w, h);
}

void x11_server_emit_window_destroy(uint32_t xid)
{
  x11_emit_window_destroy(xid);
}

void x11_server_apply_map_request(uint32_t xid)
{
  if (xid == 0) return;

  int did_map = 0;

  x11_backend_lock();
  if (x11_backend_window_exists_locked(xid) &&
      !x11_backend_window_is_closing_locked(xid) &&
      !x11_backend_window_is_mapped_locked(xid))
  {
    (void)x11_backend_window_set_mapped_locked(xid, 1);
    x11_backend_mark_damage_locked(xid);
    did_map = 1;
  }
  x11_backend_unlock();

  if (!did_map) return;

  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_MAP;
  ev.size = sizeof(ev.u.win_map);
  (void)x11_events_push(&ev);

  x11_server_wakeup();
}


void x11_server_apply_unmap_request(uint32_t xid)
{
  if (xid == 0) return;

  int did_unmap = 0;

  x11_backend_lock();
  if (x11_backend_window_exists_locked(xid) &&
      x11_backend_window_is_mapped_locked(xid))
  {
    (void)x11_backend_window_set_mapped_locked(xid, 0);
    did_unmap = 1;
  }
  x11_backend_unlock();

  if (!did_unmap) return;

  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_UNMAP;
  ev.size = sizeof(ev.u.win_unmap);
  (void)x11_events_push(&ev);
}


void x11_server_apply_configure_request(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (xid == 0) return;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  int did = 0;
  int mapped = 0;

  x11_backend_lock();
  if (x11_backend_window_exists_locked(xid) &&
      !x11_backend_window_is_closing_locked(xid))
  {
    // Apply geometry always (even if unmapped)
    x11_backend_window_set_size_locked(xid, w_px, h_px);
    mapped = x11_backend_window_is_mapped_locked(xid);

    // If mapped, schedule repaint
    if (mapped) x11_backend_mark_damage_locked(xid);
    did = 1;
  }
  x11_backend_unlock();

  if (!did) return;

  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_RESIZE;
  ev.size = sizeof(ev.u.win_resize);
  ev.u.win_resize.width_px  = w_px;
  ev.u.win_resize.height_px = h_px;
  (void)x11_events_push(&ev);

  if (mapped) x11_server_wakeup();
}



