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

#include "x11_shim.h"
#include "x11_events.h"   // so we can enqueue x11_event_t

static const uint32_t XID_A = 0x10001;
static const uint32_t XID_B = 0x10002;

static x11_window_created_cb s_on_create = 0;
static x11_window_closed_cb  s_on_close  = 0;
static x11_present_frame_cb    s_present = 0;


// ---- Minimal backend window table + damage
#define X11_MAX_WINDOWS 64

typedef struct {
  uint32_t xid;
  int32_t  w_px;
  int32_t  h_px;
  
  uint8_t  alive;
  uint8_t  damaged;

  uint8_t  closing;                  // window is being destroyed
  _Atomic uint32_t repaint_inflight; // number of active repaints

  uint32_t *fb;                      // framebuffer
  size_t    fb_cap_pixels;           // capacity in pixels
} x11_win_state_t;


static x11_win_state_t g_windows[X11_MAX_WINDOWS];

#ifndef NDEBUG
static void x11_debug_check_slot_invariants_locked(int i) {
  // Must be called with g_mu held.
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

static void x11_debug_check_all_invariants_locked(void) {
  for (int i = 0; i < X11_MAX_WINDOWS; i++) {
    x11_debug_check_slot_invariants_locked(i);
  }
}
#endif

// ---- Runloop thread + wakeup
static pthread_t g_thread;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv;
static int             g_cv_inited = 0;

static pthread_cond_t  g_inflight_cv;
static int             g_inflight_cv_inited = 0;

static int g_runloop_running = 0;
static int g_runloop_stop = 0;

static int find_slot(uint32_t xid) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_windows[i].alive && g_windows[i].xid == xid) return i;
    }
    return -1;
}

static int alloc_slot(uint32_t xid) {
  int idx = find_slot(xid);
  if (idx >= 0) return idx;
  for (int i = 0; i < X11_MAX_WINDOWS; i++) {
    if (!g_windows[i].alive) {
      g_windows[i].alive = 1;
      g_windows[i].xid = xid;
      g_windows[i].w_px = 1;
      g_windows[i].h_px = 1;
      g_windows[i].damaged = 1;
      g_windows[i].fb = NULL;
      g_windows[i].fb_cap_pixels = 0;
      atomic_store_explicit(&g_windows[i].repaint_inflight, 0, memory_order_relaxed);
      g_windows[i].closing = 0;
#ifndef NDEBUG
      x11_debug_check_slot_invariants_locked(i);
#endif
      return i;
    }
  }
  return -1;
}

static uint32_t g_pointer_xid = 0;
static uint32_t g_focus_xid = 0;
static uint32_t g_drag_xid    = 0;
static uint32_t g_buttons     = 0;   // current button mask

// ---- Debug: repaint storm injector (useful for stop/destroy race testing)
static int      g_debug_storm = 0;
static uint32_t g_debug_storm_xid = 0;
static int      g_debug_destroy_during_repaint = 0;
static uint32_t g_debug_destroy_during_repaint_xid = 0;

// Debug: count how many destroys had to wait for in-flight repaints
static _Atomic uint64_t g_debug_destroy_waits = 0;

void x11_debug_set_repaint_storm(int enabled, uint32_t xid)
{
  pthread_mutex_lock(&g_mu);
  g_debug_storm = enabled ? 1 : 0;
  g_debug_storm_xid = xid;

  // Kick the first repaint immediately if possible.
  if (g_debug_storm && g_debug_storm_xid != 0) {
    int idx = find_slot(g_debug_storm_xid);
    if (idx >= 0 && g_windows[idx].alive && !g_windows[idx].closing) {
      g_windows[idx].damaged = 1;
    }
  }
  
#ifndef NDEBUG
  x11_debug_check_all_invariants_locked();
#endif

  pthread_mutex_unlock(&g_mu);
  x11_server_wakeup();
}

void x11_debug_destroy_during_next_repaint(int enabled, uint32_t xid)
{
  pthread_mutex_lock(&g_mu);
  g_debug_destroy_during_repaint = enabled ? 1 : 0;
  g_debug_destroy_during_repaint_xid = xid;
  pthread_mutex_unlock(&g_mu);
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


static inline bool any_buttons_down(uint32_t buttons) {
    return buttons != 0;
}

// ---- Helper for async destroy (safe from repaint context)
struct x11_destroy_arg { uint32_t xid; };

static void x11_emit_window_create(uint32_t xid, const char* title, int32_t w, int32_t h)
{
  // ensure nonzero
  int32_t ww = (w < 1) ? 1 : w;
  int32_t hh = (h < 1) ? 1 : h;

  // Ensure backend state exists + mark damaged + wake runloop
  x11_set_window_size(xid, ww, hh);
  
  // Ask Swift to create the NSWindow
  x11_window_created_cb on_create = NULL;
  pthread_mutex_lock(&g_mu);
  on_create = s_on_create;
  pthread_mutex_unlock(&g_mu);

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

}


static void x11_emit_window_destroy(uint32_t xid)
{
  x11_window_closed_cb on_close = NULL;
  uint32_t *old_fb = NULL;

  pthread_mutex_lock(&g_mu);
  int idx = find_slot(xid);

  // idempotent destroy
  if (idx < 0) {
    pthread_mutex_unlock(&g_mu);
    return;
  }

  // 0) If we’re doing the repaint-storm test, stop generating new damage now.
  // This makes the test one-shot and keeps the system calm after the destroy.
  g_debug_storm = 0;
  g_debug_storm_xid = 0;
  
  // 1) Prevent *new* repaints from starting on this window.
  g_windows[idx].closing = 1;
  g_windows[idx].damaged = 0;

  // 2) Clear routing ownership while we still know xid
  if (g_pointer_xid == xid) g_pointer_xid = 0;
  if (g_focus_xid == xid)   g_focus_xid = 0;
  if (g_drag_xid == xid)    g_drag_xid = 0;

  // 3) Snapshot callback
  on_close = s_on_close;

  // 4) Detach framebuffer pointer, but DO NOT FREE YET.
  old_fb = g_windows[idx].fb;
  g_windows[idx].fb = NULL;
  g_windows[idx].fb_cap_pixels = 0;

  // 5) Wait for in-flight repaints to finish
  uint32_t inflight = atomic_load_explicit(&g_windows[idx].repaint_inflight, memory_order_relaxed);
  if (inflight != 0) {
    atomic_fetch_add_explicit(&g_debug_destroy_waits, 1, memory_order_relaxed);
  #ifndef NDEBUG
    fprintf(stderr, "[SwiftX11] destroy xid=0x%X waiting inflight=%u\n", xid, inflight);
  #endif
  }
  while (atomic_load_explicit(&g_windows[idx].repaint_inflight, memory_order_relaxed) != 0) {
    if (g_inflight_cv_inited) {
      pthread_cond_wait(&g_inflight_cv, &g_mu);
    } else {
      // Fallback if the CV isn't initialized for some reason.
      pthread_mutex_unlock(&g_mu);
      usleep(1000);
      pthread_mutex_lock(&g_mu);
    }
  }
  
  // 6) Now it is safe to fully clear the slot (no repaint can touch it anymore)
  g_windows[idx].xid = 0;
  g_windows[idx].w_px = 0;
  g_windows[idx].h_px = 0;
  g_windows[idx].closing = 0;
  g_windows[idx].alive = 0;

#ifndef NDEBUG
  x11_debug_check_slot_invariants_locked(idx);
#endif

  pthread_mutex_unlock(&g_mu);

  // Free outside lock
  if (old_fb) free(old_fb);

  // Enqueue event as backend truth
  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_DESTROY;
  ev.size = sizeof(ev.u.win_destroy);
  (void)x11_events_push(&ev);

  // Ask Swift to actually close the NSWindow (NO LOCK held)
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

// ---- Debug helpers
void x11_debug_dump_window_table(void)
{
    pthread_mutex_lock(&g_mu);

    fprintf(stderr, "\n[SwiftX11] Window table dump:\n");
    fprintf(stderr, "  pointer_xid=0x%X focus_xid=0x%X drag_xid=0x%X buttons=0x%X storm=%d storm_xid=0x%X\n",
            g_pointer_xid, g_focus_xid, g_drag_xid, g_buttons, g_debug_storm, g_debug_storm_xid);

    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (!g_windows[i].alive) continue;
        fprintf(stderr,
                "  slot=%d xid=0x%X size=%dx%d damaged=%u\n",
                i,
                g_windows[i].xid,
                (int)g_windows[i].w_px,
                (int)g_windows[i].h_px,
                (unsigned)g_windows[i].damaged);
    }

    fprintf(stderr, "[SwiftX11] End window table dump.\n\n");

    pthread_mutex_unlock(&g_mu);
}

int x11_debug_get_window_alive(uint32_t xid)
{
    pthread_mutex_lock(&g_mu);
    int idx = find_slot(xid);
    int alive = (idx >= 0) ? 1 : 0;
    pthread_mutex_unlock(&g_mu);
    return alive;
}

int x11_debug_get_window_size(uint32_t xid, int32_t* out_w_px, int32_t* out_h_px)
{
    if (!out_w_px || !out_h_px) return 0;

    pthread_mutex_lock(&g_mu);
    int idx = find_slot(xid);
    if (idx < 0) {
        pthread_mutex_unlock(&g_mu);
        return 0;
    }

    *out_w_px = g_windows[idx].w_px;
    *out_h_px = g_windows[idx].h_px;
    pthread_mutex_unlock(&g_mu);
    return 1;
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
    pthread_mutex_lock(&g_mu);

    out_snapshot->pointer_xid = g_pointer_xid;
    out_snapshot->focus_xid   = g_focus_xid;
    out_snapshot->drag_xid    = g_drag_xid;
    out_snapshot->buttons     = g_buttons;
    out_snapshot->destroy_waits =
      atomic_load_explicit(&g_debug_destroy_waits, memory_order_relaxed);
  
    uint32_t n = 0;
    for (int i = 0; i < X11_MAX_WINDOWS && n < X11_DEBUG_MAX_WINDOWS; i++) {
        if (!g_windows[i].alive) continue;

        out_snapshot->windows[n].xid     = g_windows[i].xid;
        out_snapshot->windows[n].w_px    = g_windows[i].w_px;
        out_snapshot->windows[n].h_px    = g_windows[i].h_px;
        out_snapshot->windows[n].alive   = g_windows[i].alive;
        out_snapshot->windows[n].damaged = g_windows[i].damaged;
        n++;
    }
    out_snapshot->window_count = n;

    pthread_mutex_unlock(&g_mu);
    return 1;
}

void x11_register_callbacks(
                            x11_window_created_cb on_create,
                            x11_window_closed_cb on_close)
{
  pthread_mutex_lock(&g_mu);
  s_on_create = on_create;
  s_on_close  = on_close;
  pthread_mutex_unlock(&g_mu);
}

bool x11_start_server(int32_t display)
{
  (void)display;

  // mouse and keyboard event handling
  x11_events_init();

  // Create two test windows (backend alloc + Swift callback + event + damage)
  x11_emit_window_create(XID_A, "Test X11 Window A", 800, 600);
  x11_emit_window_create(XID_B, "Test X11 Window B", 520, 360);

#if 0
  x11_debug_dump_window_table();
#endif

  // Start repaint runloop
  x11_server_runloop_start();

  return true;
}

void x11_stop_server(void)
{
  // Snapshot live windows under lock (avoid iterating while mutating)
  uint32_t live[X11_MAX_WINDOWS];
  int n = 0;
  
  pthread_mutex_lock(&g_mu);
  for (int i = 0; i < X11_MAX_WINDOWS; i++) {
    if (g_windows[i].alive && n < X11_MAX_WINDOWS) {
      live[n++] = g_windows[i].xid;
    }
  }
  pthread_mutex_unlock(&g_mu);
  
  // Close all live windows (idempotent destroy makes this safe)
  for (int i = 0; i < n; i++) {
    x11_emit_window_destroy(live[i]);
  }
  
#if 0
  x11_debug_dump_window_table();
#endif
  
  x11_server_runloop_stop();
  x11_events_shutdown();
}


void x11_register_frame_presenter(x11_present_frame_cb on_present)
{
  pthread_mutex_lock(&g_mu);
  s_present = on_present;

  // When the presenter becomes available, mark all live windows damaged
  // so their first frame is guaranteed to be produced.
  if (s_present) {
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
      if (g_windows[i].alive) {
        g_windows[i].damaged = 1;
      }
    }
  }

  pthread_mutex_unlock(&g_mu);

  // Wake the runloop so we repaint immediately (rather than waiting for timeout).
  x11_server_wakeup();
}


void x11_request_repaint(uint32_t xwin_id, int32_t width_px, int32_t height_px)
{
    if (width_px <= 0 || height_px <= 0) return;

    x11_present_frame_cb presenter = NULL;
    int idx = -1;
    uint32_t *fb = NULL;
    size_t cap = 0;

    // We must decrement inflight on every exit if we increment it.
    bool inflight_taken = false;

    // 1) Snapshot presenter + find window + block if closing, then take inflight
    pthread_mutex_lock(&g_mu);
    presenter = s_present;
    idx = find_slot(xwin_id);

    if (!presenter || idx < 0 || !g_windows[idx].alive || g_windows[idx].xid != xwin_id || g_windows[idx].closing) {
        pthread_mutex_unlock(&g_mu);
        return;
    }

    atomic_fetch_add_explicit(&g_windows[idx].repaint_inflight, 1, memory_order_relaxed);
    inflight_taken = true;
#ifndef NDEBUG
    x11_debug_check_slot_invariants_locked(idx);
#endif

    fb  = g_windows[idx].fb;
    cap = g_windows[idx].fb_cap_pixels;
    pthread_mutex_unlock(&g_mu);
  
    // debugging -- slow down the repaint
    if (g_debug_storm) { usleep(20 * 1000); }  // 20ms, tune 1–20ms

  // Deterministic destroy-while-inflight test (one-shot)
  int do_destroy = 0;
  uint32_t dxid = 0;
  pthread_mutex_lock(&g_mu);
  if (g_debug_destroy_during_repaint &&
      g_debug_destroy_during_repaint_xid == xwin_id)
  {
    do_destroy = 1;
    dxid = g_debug_destroy_during_repaint_xid;
    g_debug_destroy_during_repaint = 0; // one-shot
  }
  pthread_mutex_unlock(&g_mu);

  if (do_destroy) {
    // IMPORTANT: never call synchronous destroy from inside repaint
    // (we hold repaint_inflight and would deadlock waiting for it to hit 0).
    x11_post_window_destroy_async(dxid);
  }

    // 2) Ensure capacity (grow outside lock, then swap under lock if still valid)
    const size_t need_pixels = (size_t)width_px * (size_t)height_px;
    const int bpr = width_px * 4;

    if (cap < need_pixels) {
        uint32_t *new_fb = (uint32_t*)malloc(need_pixels * sizeof(uint32_t));
        if (!new_fb) goto done;

        uint32_t *old_fb = NULL;
        bool swapped = false;

        pthread_mutex_lock(&g_mu);
        // Revalidate: still same window, alive, not closing
        if (idx >= 0 &&
            g_windows[idx].alive &&
            g_windows[idx].xid == xwin_id &&
            !g_windows[idx].closing)
        {
            old_fb = g_windows[idx].fb;
            g_windows[idx].fb = new_fb;
            g_windows[idx].fb_cap_pixels = need_pixels;
            fb = new_fb;
            cap = need_pixels;
            new_fb = NULL;     // ownership transferred
            swapped = true;
        }
        pthread_mutex_unlock(&g_mu);

        if (new_fb) free(new_fb);  // window died/closed before swap
        if (old_fb) free(old_fb);  // replaced old buffer

        if (!swapped || !fb) goto done; // window no longer valid
    }

    // 3) Render into fb (no lock held)
    for (int y = 0; y < height_px; y++) {
        for (int x = 0; x < width_px; x++) {
            uint8_t a = 0xFF;
            uint8_t r, g, b;

            if (xwin_id == XID_A) {
                b = (uint8_t)(x & 0xFF);
                g = (uint8_t)((y * 2) & 0xFF);
                r = (uint8_t)((x ^ y) & 0xFF);
            } else {
                b = (uint8_t)((y * 3) & 0xFF);
                g = (uint8_t)((x * 2) & 0xFF);
                r = (uint8_t)(200);
                if ((x / 20) % 2) { g = 255; }
            }

            fb[(size_t)y * (size_t)width_px + (size_t)x] =
                (uint32_t)(b | (g << 8) | (r << 16) | (a << 24));
        }
    }

    // 4) Present (no lock held)
    presenter(xwin_id, fb, width_px, height_px, bpr);

done:
    // 5) Drop inflight + wake destroy waiters if needed
    if (inflight_taken) {
        pthread_mutex_lock(&g_mu);
        // Only touch counter if slot is still this xid and alive/closing state is meaningful
        if (idx >= 0 && g_windows[idx].xid == xwin_id) {
            uint32_t v = atomic_fetch_sub_explicit(&g_windows[idx].repaint_inflight, 1, memory_order_relaxed) - 1u;
            // If destroy is waiting, wake it when we hit zero.
            if (g_windows[idx].closing && v == 0 && g_inflight_cv_inited) {
                pthread_cond_broadcast(&g_inflight_cv);
            }
#ifndef NDEBUG
            x11_debug_check_slot_invariants_locked(idx);
#endif
        }
        pthread_mutex_unlock(&g_mu);
    }
}

void x11_post_pointer_event(uint32_t xid, x11_ptr_event_type type,
                            int32_t x_px, int32_t y_px,
                            uint32_t buttons, uint32_t modifiers)
{
    // Motion gating: only allow motion for the current pointer window unless a drag grab is active.
    if (type == X11_PTR_MOVE) {
        uint32_t drag = 0;
        uint32_t pointer = 0;

        pthread_mutex_lock(&g_mu);
        drag = g_drag_xid;
        pointer = g_pointer_xid;
        pthread_mutex_unlock(&g_mu);

        if (drag != 0) {
            if (drag != xid) return;
        } else {
            // Require pointer to be "inside" a window (set via enter/leave)
            if (pointer == 0) return;
            if (pointer != xid) return;
        }
    }

    // Keep canonical backend button/drag state correct even for the legacy API.
    // NOTE: Some callers historically passed 0 for motion. Therefore:
    //   - For motion: NEVER trust `buttons`; read g_buttons instead.
    //   - For down/up: treat `buttons` as the AFTER-state mask and commit it to g_buttons.
    uint32_t buttons_snapshot = 0;

    if (type == X11_PTR_DOWN || type == X11_PTR_UP) {
        pthread_mutex_lock(&g_mu);

        // Canonical after-state button mask
        g_buttons = buttons;

        // Drag grab behavior: first press grabs; releasing last button clears.
        if (type == X11_PTR_DOWN) {
            if (g_drag_xid == 0) g_drag_xid = xid;
        } else {
            if (!any_buttons_down(buttons)) g_drag_xid = 0;
        }

        buttons_snapshot = g_buttons;
        pthread_mutex_unlock(&g_mu);
    } else if (type == X11_PTR_MOVE) {
        pthread_mutex_lock(&g_mu);
        buttons_snapshot = g_buttons;
        pthread_mutex_unlock(&g_mu);
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
    pthread_mutex_lock(&g_mu);
    target = g_focus_xid;
    pthread_mutex_unlock(&g_mu);
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
  // Track drag ownership: first press grabs, last release clears.
  pthread_mutex_lock(&g_mu);

  if (is_press) {
      // If no drag grab yet, grab to the window that got the press.
      if (g_drag_xid == 0) g_drag_xid = xid;
  } else {
      // If releasing and buttons becomes 0, clear grab.
      if (!any_buttons_down(buttons)) g_drag_xid = 0;
  }

  g_buttons = buttons;

  pthread_mutex_unlock(&g_mu);

  x11_event_t ev = {0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid  = xid;
  ev.type = X11_EV_POINTER_BUTTON;
  ev.size = sizeof(ev.u.button);
  
  ev.u.button.x_px = x_px;
  ev.u.button.y_px = y_px;
  ev.u.button.button = button;
  ev.u.button.is_press = is_press ? 1 : 0;
  ev.u.button.buttons = buttons;
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
  x11_event_t ev = {0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid  = xid;
  ev.type = X11_EV_SCROLL;
  ev.size = sizeof(ev.u.scroll);
  
  ev.u.scroll.x_px = x_px;
  ev.u.scroll.y_px = y_px;
  ev.u.scroll.axis = (uint8_t)axis; // store as byte in the struct
  ev.u.scroll.ticks = ticks;
  ev.u.scroll.buttons = buttons;
  ev.u.scroll.modifiers = modifiers;
  
  (void)x11_events_push(&ev);
}

void x11_post_focus_event(uint32_t xid, bool focused)
{
  pthread_mutex_lock(&g_mu);
  if (focused) {
    g_focus_xid = xid;
  } else {
    if (g_focus_xid == xid) g_focus_xid = 0;
  }
  pthread_mutex_unlock(&g_mu);
  
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
  pthread_mutex_lock(&g_mu);
  g_pointer_xid = xid;
  pthread_mutex_unlock(&g_mu);
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
  pthread_mutex_lock(&g_mu);
  if (g_drag_xid == 0 && g_pointer_xid == xid) {
    g_pointer_xid = 0;
  }
  pthread_mutex_unlock(&g_mu);
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
    // Synchronous destroy. Safe from normal UI/control paths.
    x11_emit_window_destroy(xid);
}


void x11_post_window_destroy_async(uint32_t xid)
{
    // Asynchronous destroy: safe to call from contexts that must not block.
    x11_emit_window_destroy_async(xid);
}


void x11_set_window_size(uint32_t xid, int32_t width_px, int32_t height_px)
{
  if (width_px < 1) width_px = 1;
  if (height_px < 1) height_px = 1;
  
  pthread_mutex_lock(&g_mu);
  int idx = find_slot(xid);
  if (idx < 0) idx = alloc_slot(xid);
  
  if (idx >= 0 && !g_windows[idx].closing) {
    g_windows[idx].w_px = width_px;
    g_windows[idx].h_px = height_px;
    g_windows[idx].damaged = 1;
  }
  pthread_mutex_unlock(&g_mu);
  
  x11_server_wakeup();
}

void x11_mark_damage(uint32_t xid)
{
  pthread_mutex_lock(&g_mu);
  int idx = find_slot(xid);
  if (idx < 0) idx = alloc_slot(xid);
  
  if (idx >= 0 && !g_windows[idx].closing) {
    g_windows[idx].damaged = 1;
  }
  pthread_mutex_unlock(&g_mu);
  
  x11_server_wakeup();
}

void x11_server_wakeup(void)
{
  pthread_mutex_lock(&g_mu);
  if (g_runloop_running && g_cv_inited) {
    pthread_cond_signal(&g_cv);
  }
  pthread_mutex_unlock(&g_mu);
}

// process one “tick”: snapshot damaged windows and repaint them.
void x11_server_step(void)
{
  // Snapshot damaged windows under lock, and clear damage before repainting.
  uint32_t xids[X11_MAX_WINDOWS];
  int32_t  ws[X11_MAX_WINDOWS];
  int32_t  hs[X11_MAX_WINDOWS];
  int n = 0;

  pthread_mutex_lock(&g_mu);
  for (int i = 0; i < X11_MAX_WINDOWS; i++) {
    if (g_windows[i].alive && g_windows[i].damaged && !g_windows[i].closing) {
      if (n >= X11_MAX_WINDOWS) break;

      g_windows[i].damaged = 0;

      xids[n] = g_windows[i].xid;
      ws[n]   = g_windows[i].w_px;
      hs[n]   = g_windows[i].h_px;
      n++;
    }
  }
  // Debug storm: keep one window continuously damaged so the runloop repaints every tick.
  // This is intentionally simple and is meant for stress-testing stop/destroy while repaints are active.
  if (g_debug_storm && g_debug_storm_xid != 0) {
    int si = find_slot(g_debug_storm_xid);
    if (si >= 0 && g_windows[si].alive && !g_windows[si].closing) {
      g_windows[si].damaged = 1;
    }
  }
#ifndef NDEBUG
  x11_debug_check_all_invariants_locked();
#endif
  pthread_mutex_unlock(&g_mu);

  // Perform repaints outside lock
  for (int i = 0; i < n; i++) {
    x11_request_repaint(xids[i], ws[i], hs[i]);
  }
}

static void* runloop_main(void* _)
{
  (void)_;
    pthread_mutex_lock(&g_mu);

  while (!g_runloop_stop) {
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
    (void)pthread_cond_timedwait(&g_cv, &g_mu, &ts);

    pthread_mutex_unlock(&g_mu);

    x11_server_step();

    pthread_mutex_lock(&g_mu);

  }

    pthread_mutex_unlock(&g_mu);
    return NULL;
}

void x11_server_runloop_start(void)
{
  pthread_mutex_lock(&g_mu);
  
  // initialize g_cv
  int rc;
  if (!g_cv_inited) {
    pthread_condattr_t attr;
    rc = pthread_condattr_init(&attr);
#ifndef NDEBUG
    assert(rc == 0);
#else
    (void)rc;
#endif

    // NOTE: On macOS, condition variables use CLOCK_REALTIME for timed waits.
    rc = pthread_cond_init(&g_cv, &attr);
#ifndef NDEBUG
    assert(rc == 0);
#else
    (void)rc;
#endif

    pthread_condattr_destroy(&attr);
    g_cv_inited = 1;
  }

  if (!g_inflight_cv_inited) {
      pthread_condattr_t attr2;
      int rc2 = pthread_condattr_init(&attr2);
  #ifndef NDEBUG
      assert(rc2 == 0);
  #else
      (void)rc2;
  #endif
      // macOS uses CLOCK_REALTIME; no setclock.
      rc2 = pthread_cond_init(&g_inflight_cv, &attr2);
  #ifndef NDEBUG
      assert(rc2 == 0);
  #else
      (void)rc2;
  #endif
      pthread_condattr_destroy(&attr2);
      g_inflight_cv_inited = 1;
  }
  
  if (g_runloop_running) {
    pthread_mutex_unlock(&g_mu);
    return;
  }
  g_runloop_stop = 0;
  g_runloop_running = 1;
  pthread_mutex_unlock(&g_mu);

  pthread_create(&g_thread, NULL, runloop_main, NULL);
}

void x11_server_runloop_stop(void)
{
    pthread_mutex_lock(&g_mu);
    if (!g_runloop_running) {
        pthread_mutex_unlock(&g_mu);
        return;
    }
    g_runloop_stop = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);

    pthread_join(g_thread, NULL);

    pthread_mutex_lock(&g_mu);
    g_runloop_running = 0;
    pthread_mutex_unlock(&g_mu);
  
  if (g_cv_inited) {
    pthread_cond_destroy(&g_cv);
    g_cv_inited = 0;
  }
  
  if (g_inflight_cv_inited) {
      pthread_cond_destroy(&g_inflight_cv);
      g_inflight_cv_inited = 0;
  }
}
