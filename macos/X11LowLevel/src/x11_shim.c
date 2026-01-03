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
} x11_win_state_t;

static x11_win_state_t g_windows[X11_MAX_WINDOWS];

// ---- Runloop thread + wakeup
static pthread_t g_thread;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv;
static int             g_cv_inited = 0;

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
            return i;
        }
    }
    return -1;
}
static uint32_t g_pointer_xid = 0;
static uint32_t g_focus_xid = 0;
static uint32_t g_drag_xid    = 0;
static uint32_t g_buttons     = 0;   // current button mask

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

static void x11_emit_window_create(uint32_t xid, const char* title, int32_t w, int32_t h)
{
  // ensure nonzero
  int32_t ww = (w < 1) ? 1 : w;
  int32_t hh = (h < 1) ? 1 : h;

  // Ensure backend state exists + mark damaged + wake runloop
  x11_set_window_size(xid, ww, hh);
  
  // Ask Swift to create the NSWindow
  if (s_on_create) {
    s_on_create(xid, title, (int)ww, (int)hh);
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
  // Clear backend state
  pthread_mutex_lock(&g_mu);
  int idx = find_slot(xid);
  
  // NEW: idempotent destroy — if it's already gone, do nothing
  if (idx < 0) {
    pthread_mutex_unlock(&g_mu);
    return;
  }
  
  // Fully clear the slot so stale state can't be observed later.
  g_windows[idx].alive   = 0;
  g_windows[idx].damaged = 0;
  g_windows[idx].xid     = 0;
  g_windows[idx].w_px    = 0;
  g_windows[idx].h_px    = 0;

  pthread_mutex_unlock(&g_mu);
  
  // Enqueue event as backend truth
  x11_event_t ev = (x11_event_t){0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_DESTROY;
  ev.size = sizeof(ev.u.win_destroy);
  (void)x11_events_push(&ev);
  
  // Ask Swift to actually close the NSWindow
  if (s_on_close) {
    s_on_close(xid);
  }
  
  // If pointer/focus/drag were targeting this window, clear them.
  if (g_pointer_xid == xid) g_pointer_xid = 0;
  if (g_focus_xid == xid)   g_focus_xid = 0;
  if (g_drag_xid == xid)    g_drag_xid = 0;
  
  x11_server_wakeup();
}

// ---- Debug helpers
void x11_debug_dump_window_table(void)
{
    pthread_mutex_lock(&g_mu);

    fprintf(stderr, "\n[SwiftX11] Window table dump:\n");
    fprintf(stderr, "  pointer_xid=0x%X focus_xid=0x%X drag_xid=0x%X buttons=0x%X\n",
            g_pointer_xid, g_focus_xid, g_drag_xid, g_buttons);

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


void x11_register_callbacks(
                            x11_window_created_cb on_create,
                            x11_window_closed_cb on_close)
{
  s_on_create = on_create;
  s_on_close  = on_close;
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
    if (g_windows[i].alive) {
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

void x11_register_frame_presenter(x11_present_frame_cb on_present) {
  s_present = on_present;
}

// Simple BGRA test pattern generator that matches the requested size.
void x11_request_repaint(uint32_t xwin_id, int32_t width_px, int32_t height_px)
{
  if (!s_present) return;
  if (width_px <= 0 || height_px <= 0) return;
  
  const int bpr = width_px * 4;
  const size_t count = (size_t)width_px * (size_t)height_px;
  
  // Allocate a temporary buffer per repaint (fine for testing).
  uint32_t *buf = (uint32_t*)malloc(count * sizeof(uint32_t));
  if (!buf) return;
  
  for (int y = 0; y < height_px; y++) {
    for (int x = 0; x < width_px; x++) {
      uint8_t a = 0xFF;

      uint8_t r, g, b;
      if (xwin_id == XID_A) {
        // Window A: your existing gradient
        b = (uint8_t)(x & 0xFF);
        g = (uint8_t)((y * 2) & 0xFF);
        r = (uint8_t)((x ^ y) & 0xFF);
      } else {
        // Window B: different obvious pattern (vertical bars + red bias)
        b = (uint8_t)((y * 3) & 0xFF);
        g = (uint8_t)((x * 2) & 0xFF);
        r = (uint8_t)(200);
        if ((x / 20) % 2) { g = 255; } // bright bars
      }

      buf[(size_t)y * (size_t)width_px + (size_t)x] =
        (uint32_t)(b | (g << 8) | (r << 16) | (a << 24));
    }
  }
  
  s_present(xwin_id, buf, width_px, height_px, bpr);
  
  free(buf);
}

void x11_post_pointer_event(uint32_t xid, x11_ptr_event_type type,
                            int32_t x_px, int32_t y_px,
                            uint32_t buttons, uint32_t modifiers)
{
    // Motion gating: only allow motion for the current pointer window unless a drag grab is active.
    if (type == X11_PTR_MOVE) {
        if (g_drag_xid != 0) {
            if (g_drag_xid != xid) return;
        } else {
            // Require pointer to be "inside" a window (set via enter/leave)
            if (g_pointer_xid == 0) return;
            if (g_pointer_xid != xid) return;
        }
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
        ev.u.motion.buttons = buttons;
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
        ev.u.button.buttons = buttons;
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
  if (target == 0) target = g_focus_xid;

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
  if (is_press) {
      // If no drag grab yet, grab to the window that got the press.
      if (g_drag_xid == 0) g_drag_xid = xid;
  } else {
      // If releasing and buttons becomes 0, clear grab.
      if (!any_buttons_down(buttons)) g_drag_xid = 0;
  }

  g_buttons = buttons;

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
  if (focused) {
    g_focus_xid = xid;
  } else {
    if (g_focus_xid == xid) g_focus_xid = 0;
  }
  
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
  g_pointer_xid = xid;
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
  if (g_drag_xid == 0 && g_pointer_xid == xid) g_pointer_xid = 0;
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
    x11_emit_window_destroy(xid);
}

void x11_set_window_size(uint32_t xid, int32_t width_px, int32_t height_px)
{
    if (width_px < 1) width_px = 1;
    if (height_px < 1) height_px = 1;

    pthread_mutex_lock(&g_mu);
    int idx = alloc_slot(xid);
    if (idx >= 0) {
        g_windows[idx].w_px = width_px;
        g_windows[idx].h_px = height_px;
        g_windows[idx].damaged = 1;
        // x11_debug_dump_window_table();
    }
    pthread_mutex_unlock(&g_mu);

    x11_server_wakeup();
}

void x11_mark_damage(uint32_t xid)
{
    pthread_mutex_lock(&g_mu);
    int idx = alloc_slot(xid);
    if (idx >= 0) {
        g_windows[idx].damaged = 1;
        // x11_debug_dump_window_table();
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

// NEW: process one “tick”: snapshot damaged windows and repaint them.
void x11_server_step(void)
{
    // Snapshot damaged windows under lock
    uint32_t xids[X11_MAX_WINDOWS];
    int32_t  ws[X11_MAX_WINDOWS];
    int32_t  hs[X11_MAX_WINDOWS];
    int n = 0;

    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < X11_MAX_WINDOWS; i++) {
        if (g_windows[i].alive && g_windows[i].damaged) {
            g_windows[i].damaged = 0;
            xids[n] = g_windows[i].xid;
            ws[n]   = g_windows[i].w_px;
            hs[n]   = g_windows[i].h_px;
            n++;
        }
    }
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
}
