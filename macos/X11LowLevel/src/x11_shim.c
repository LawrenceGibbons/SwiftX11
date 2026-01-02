#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach_time.h>

#include "x11_shim.h"
#include <x11_events.h>   // so we can enqueue x11_event_t

static x11_window_created_cb s_on_create = 0;
static x11_window_closed_cb  s_on_close  = 0;
static x11_present_frame_cb    s_present = 0;
static uint32_t g_pointer_xid = 0;
static uint32_t g_focus_xid = 0;

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
  
  // Test window
  const uint32_t xid = 0x10001;
  const int32_t w = 800;
  const int32_t h = 600;
  
  x11_event_t ev = {0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_CREATE;
  ev.size = sizeof(ev.u.win_create);
  ev.u.win_create.width_px = w;
  ev.u.win_create.height_px = h;
  x11_events_push(&ev);
  
  if (s_on_create) {
    s_on_create(xid, "Test X11 Window", w, h);
  }
  
  // Initial paint (uses the same path as resize repaint)
  x11_request_repaint(0x10001, 800, 600);
  
  return true;
}

void x11_stop_server(void)
{
  const uint32_t xid = 0x10001;
  
  x11_event_t ev = {0};
  ev.timestamp_ns = x11_now_ns();
  ev.xid = xid;
  ev.type = X11_EV_WINDOW_DESTROY;
  ev.size = sizeof(ev.u.win_destroy);
  x11_events_push(&ev);
  
  if (s_on_close) {
    s_on_close(xid);
  }
  
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
      uint8_t b = (uint8_t)(x & 0xFF);
      uint8_t g = (uint8_t)((y * 2) & 0xFF);
      uint8_t r = (uint8_t)((x ^ y) & 0xFF);
      uint8_t a = 0xFF;
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
  if (type == X11_PTR_MOVE) {
    if (g_pointer_xid != 0 && g_pointer_xid != xid) {
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
  if (g_pointer_xid == xid) g_pointer_xid = 0;
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

