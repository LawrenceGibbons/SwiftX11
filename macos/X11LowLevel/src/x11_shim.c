#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <mach/mach_time.h>

#include "x11_shim.h"
#include <x11_events.h>   // so we can enqueue x11_event_t

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
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;

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
      
  if (s_on_create) {
    s_on_create(XID_A, "Test X11 Window A", 800, 600);
    s_on_create(XID_B, "Test X11 Window B", 520, 360);
  }
  
  // Enqueue WINDOW_CREATE events (backend truth)
  x11_event_t evA = {0};
  evA.timestamp_ns = x11_now_ns();
  evA.xid = XID_A;
  evA.type = X11_EV_WINDOW_CREATE;
  evA.size = sizeof(evA.u.win_create);
  evA.u.win_create.width_px = 800;
  evA.u.win_create.height_px = 600;
  x11_events_push(&evA);

  x11_event_t evB = {0};
  evB.timestamp_ns = x11_now_ns();
  evB.xid = XID_B;
  evB.type = X11_EV_WINDOW_CREATE;
  evB.size = sizeof(evB.u.win_create);
  evB.u.win_create.width_px = 520;
  evB.u.win_create.height_px = 360;
  x11_events_push(&evB);

  
  // force the windows to repaint by marking damage
  x11_set_window_size(XID_A, 800, 600);
  x11_mark_damage(XID_A);
  
  x11_set_window_size(XID_B, 520, 360);
  x11_mark_damage(XID_B);
  
  x11_server_runloop_start();
  
  return true;
}

void x11_stop_server(void)
{
  // Emit destroy events for every window we created.
  x11_event_t evA = (x11_event_t){0};
  evA.timestamp_ns = x11_now_ns();
  evA.xid = XID_A;
  evA.type = X11_EV_WINDOW_DESTROY;
  evA.size = sizeof(evA.u.win_destroy);
  (void)x11_events_push(&evA);
  
  x11_event_t evB = (x11_event_t){0};
  evB.timestamp_ns = x11_now_ns();
  evB.xid = XID_B;
  evB.type = X11_EV_WINDOW_DESTROY;
  evB.size = sizeof(evB.u.win_destroy);
  (void)x11_events_push(&evB);
  
  // Ask Swift to actually close the NSWindows.
  if (s_on_close) {
    s_on_close(XID_A);
    s_on_close(XID_B);
  }
  
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
  // Only gate motion to the current pointer window.
  // and suppress motion unless pointer is "inside"
  if (type == X11_PTR_MOVE) {
    if (g_pointer_xid != 0 && g_pointer_xid != xid) {
      return;
    }
    if (g_pointer_xid != xid) {
        return;
    }
  }
    
  x11_event_t ev = {0};
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
    x11_events_push(&ev);
  } else if (ev.type == X11_EV_POINTER_BUTTON) {
    ev.size = sizeof(ev.u.button);
    ev.u.button.x_px = x_px;
    ev.u.button.y_px = y_px;
    // For now we can’t infer which button changed from this legacy API,
    // so leave button=0 and is_press based on type. We’ll fix this next.
    ev.u.button.button = 0;
    ev.u.button.is_press = (type == X11_PTR_DOWN) ? 1 : 0;
    ev.u.button.buttons = buttons;
    ev.u.button.modifiers = modifiers;
    x11_events_push(&ev);
  } else if (type == X11_PTR_MOVE) {
    // If a drag is active, force motion to the grabbed window.
    if (g_drag_xid != 0 && g_drag_xid != xid) {
        return;
    }

    // Otherwise, only accept motion from the current pointer window.
    if (g_drag_xid == 0 && g_pointer_xid != 0 && g_pointer_xid != xid) {
        return;
    }
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
    x11_event_t ev = (x11_event_t){0};
    ev.timestamp_ns = x11_now_ns();
    ev.xid = xid;
    ev.type = X11_EV_WINDOW_DESTROY;
    ev.size = sizeof(ev.u.win_destroy);
    (void)x11_events_push(&ev);
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
    }
    pthread_mutex_unlock(&g_mu);

    x11_server_wakeup();
}

void x11_server_wakeup(void)
{
    pthread_mutex_lock(&g_mu);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

static void* runloop_main(void* _)
{
    (void)_;
    pthread_mutex_lock(&g_mu);

    while (!g_runloop_stop) {
        // Sleep until woken (or timeout for safety)
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        // ~16ms timeout (60Hz) so we still repaint if a wake is missed
        ts.tv_nsec += 16 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }

        pthread_cond_timedwait(&g_cv, &g_mu, &ts);

        // Snapshot damaged windows under lock
        uint32_t xids[X11_MAX_WINDOWS];
        int32_t  ws[X11_MAX_WINDOWS];
        int32_t  hs[X11_MAX_WINDOWS];
        int n = 0;

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

        pthread_mutex_lock(&g_mu);
    }

    pthread_mutex_unlock(&g_mu);
    return NULL;
}

void x11_server_runloop_start(void)
{
    pthread_mutex_lock(&g_mu);
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
}
