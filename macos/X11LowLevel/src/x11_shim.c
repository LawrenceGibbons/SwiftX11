//
//  x11_shim.c
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
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "x11_parameters.h"
#include "x11_shim.h"
#include "x11_backend.h"
#include "x11_requests.h"
#include "x11_server_internal.h"
#include "x11_xproto.h"
#include "XProtoServerBridge.h"
#include "SwiftX11Bridge.h"

// -----------------------------------------------------------------------------
// Shim-local server context (scaffold step): collect shim state into one struct.
// Protected by the backend lock (x11_backend_lock/unlock).
// -----------------------------------------------------------------------------
typedef struct x11_server_ctx_t {
  // ---- Runloop thread + wakeup
  pthread_t      thread;
  pthread_cond_t cv;
  int            runloop_cv_inited;

  int runloop_running;
  int runloop_stop;

  // Debug: count how many destroys had to wait for in-flight repaints
  _Atomic uint64_t debug_destroy_waits;
  _Atomic uint64_t dbg_destroy_noop;

  // ---- X11 socket listener (early scaffold)
  int            display;
  int            x11_listen_fd;
  pthread_t      x11_listen_thread;
  int            x11_listen_running;
  int            x11_listen_stop;
  
} x11_server_ctx_t;

static x11_server_ctx_t g_srv; // zero-initialized

__attribute__((constructor))
static void x11_srv_ctor_defaults(void) {
  g_srv.x11_listen_fd = -1;
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

static void x11_emit_window_create(uint32_t xid, uint32_t parent_xid, const char* title, int32_t w, int32_t h)
{
  // ensure nonzero
  int32_t ww = (w < 1) ? 1 : w;
  int32_t hh = (h < 1) ? 1 : h;

#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] emit_window_create: xid=0x%08X title=\"%s\" size=%dx%d\n",
          (unsigned)xid, title ? title : "(null)", (int)ww, (int)hh);
#endif
  
  // Ensure backend state exists, then set size + mark damage.
  x11_backend_alloc_slot(xid);
  x11_backend_window_set_size(xid, ww, hh);
  //x11_backend_mark_damage(xid);
  x11_backend_lock();
  x11_backend_window_set_mapped_locked(xid, 0);
  x11_backend_unlock();
  
#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] emit_window_create: xid=0x%08X mapped=0 (explicit)\n",
          (unsigned)xid);
#endif
  
  x11_ui_push_create(xid, parent_xid, w, h);
  x11_server_wakeup();
}


static void x11_emit_window_destroy(uint32_t xid)
{
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
    
  // Snapshot whether the window is mapped so we can emit UNMAP before DESTROY.
  was_mapped = x11_backend_window_is_mapped_locked(xid);
  
  // Prevent *new* repaints from starting on this window.
  (void)x11_backend_window_begin_close_locked(xid);
  
  // Force mapped=0 as part of teardown semantics.
  if (was_mapped) {
    (void)x11_backend_window_set_mapped_locked(xid, 0);
  }

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

  // Free outside lock.
  if (old_fb) free(old_fb);
  if (retired) x11_backend_free_retired(retired);
  
  // If the window was mapped, emit UNMAP before DESTROY (X11-ish ordering).
  x11_ui_push_destroy(xid);
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


bool x11_start_server(int32_t display)
{
  x11_backend_lock();
  g_srv.display = (int)display;
  x11_backend_unlock();
  
  // Ensure backend process-lifetime primitives exist
  x11_backend_init();

  // Reset window table for a clean start
  x11_backend_clear_windows();
  

  // Start repaint runloop
  x11_server_runloop_start();

  // Start the (scaffold) X11 socket listener so external X11 clients can connect.
  int ok = x11_proto_start_daemon((int)display);
  return ok ? true : false;
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

  // Stop listener before stopping the runloop.
  //x11_xproto_listener_stop();
  x11_proto_stop_daemon();
  
  x11_server_runloop_stop();

  // Clear backend window table so second start is always clean
  x11_backend_clear_windows();
}

//void x11_register_frame_presenter(x11_present_frame_cb on_present)
//{
//  x11_backend_lock();
//  g_srv.present = on_present;
//  x11_backend_unlock();
//
//  // When the presenter becomes available, mark all live windows damaged
//  // so their first frame is guaranteed to be produced.
//  if (on_present) {
//    x11_backend_mark_all_damage();
//  }
//
//  // Wake the runloop so we repaint immediately (rather than waiting for timeout).
//  x11_server_wakeup();
//}

void x11_request_repaint(uint32_t xwin_id, int32_t width_px, int32_t height_px)
{
  (void)width_px;
  (void)height_px;

  // In rootless mode, Swift is responsible for presenting.
  // C side only emits DAMAGE events when the xproto framebuffer changes.

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] repaint: SKIP (rootless presenter disabled) xid=0x%08X\n",
          (unsigned)xwin_id);
#endif
}


void x11_post_pointer_event(uint32_t xid, x11_ptr_event_type type,
                            int32_t x_px, int32_t y_px,
                            uint32_t buttons, uint32_t modifiers)
{
#if !defined(NDEBUG) && SWIFTX11_TRACE
  fprintf(stderr, "[C] x11_post_pointer_event MOVE xid=0x%X x=%d y=%d buttons=0x%X mods=0x%X\n",
          xid, (int)x_px, (int)y_px, (unsigned)buttons, (unsigned)modifiers);
#endif
  
  if (xid == 0) return;

  switch (type) {
    case X11_PTR_MOVE: {
      // Legacy API: coords are window-local.
      // We cannot know true root coords here, so pass root=win.
      // deliver=1 because this is the old "deliver motion" entrypoint.
      x11_post_pointer_move2(xid,
                             x_px, y_px,      // win_x/win_y
                             x_px, y_px,      // root_x/root_y (fallback)
                             1,               // deliver
                             buttons,
                             modifiers);
      return;
    }

    case X11_PTR_DOWN:
      // Legacy API: no specific button number available here.
      // If you have x11_post_pointer_button(...) elsewhere for real button events,
      // this path is rarely used. Keep it for compatibility.
      x11_proto_bridge_post_pointer_button_legacy(xid, 1, x_px, y_px, buttons, modifiers);
      return;

    case X11_PTR_UP:
      x11_proto_bridge_post_pointer_button_legacy(xid, 0, x_px, y_px, buttons, modifiers);
      return;

    default:
      return;
  }
}


// C++ bridge (enqueue-only)
extern void x11_proto_bridge_post_pointer_move2(uint32_t xid,
                                                int32_t win_x, int32_t win_y,
                                                int32_t root_x, int32_t root_y,
                                                uint8_t deliver,
                                                uint32_t buttons,
                                                uint32_t modifiers);

extern void x11_proto_bridge_post_pointer_button(uint32_t xid,
                                                 uint8_t is_press,
                                                 uint8_t button,
                                                 int32_t win_x, int32_t win_y,
                                                 uint32_t buttons,
                                                 uint32_t modifiers);

extern void x11_proto_bridge_post_scroll(uint32_t xid,
                                         uint8_t axis,
                                         int16_t ticks,
                                         int32_t win_x, int32_t win_y,
                                         uint32_t buttons,
                                         uint32_t modifiers);

extern void x11_proto_bridge_post_key(uint32_t xid,
                                      uint8_t is_down,
                                      uint32_t keycode,
                                      uint32_t modifiers);

extern void x11_proto_bridge_post_enter(uint32_t xid,
                                        int32_t win_x, int32_t win_y,
                                        uint32_t modifiers);

extern void x11_proto_bridge_post_leave(uint32_t xid,
                                        int32_t win_x, int32_t win_y,
                                        uint32_t modifiers);

extern void x11_proto_bridge_post_focus(uint32_t xid,
                                        uint8_t focused);

void x11_post_pointer_move2(uint32_t xid,
                            int32_t win_x, int32_t win_y,
                            int32_t root_x, int32_t root_y,
                            uint8_t deliver,
                            uint32_t buttons,
                            uint32_t modifiers)
{
  if (xid == 0) return;
  x11_proto_bridge_post_pointer_move2(xid,
                                      win_x, win_y,
                                      root_x, root_y,
                                      deliver,
                                      buttons,
                                      modifiers);
}

void x11_post_key_event(uint32_t xid, bool is_down,
                        uint32_t keycode, uint32_t modifiers,
                        const char* utf8_text)
{
  (void)utf8_text;
  x11_proto_bridge_post_key(xid,
                            is_down ? 1 : 0,
                            keycode,
                            modifiers);
}


void x11_post_pointer_button(uint32_t xid,
                             bool is_press,
                             uint8_t button,
                             int32_t x_px,
                             int32_t y_px,
                             uint32_t buttons,
                             uint32_t modifiers)
{
  if (xid == 0) return;
  x11_proto_bridge_post_pointer_button(xid,
                                       is_press ? 1 : 0,
                                       button,
                                       x_px, y_px,
                                       buttons,
                                       modifiers);
}

void x11_post_scroll_ticks(uint32_t xid,
                           x11_scroll_axis_t axis,
                           int16_t ticks,
                           int32_t x_px,
                           int32_t y_px,
                           uint32_t buttons,
                           uint32_t modifiers)
{
  if (xid == 0) return;
  x11_proto_bridge_post_scroll(xid,
                               (uint8_t)axis,
                               ticks,
                               x_px, y_px,
                               buttons,
                               modifiers);
}


void x11_post_focus_event(uint32_t xid, bool focused)
{
  if (xid == 0) return;
  x11_proto_bridge_post_focus(xid, focused ? 1 : 0);
}


void x11_post_pointer_enter(uint32_t xid,
                            int32_t x_px,
                            int32_t y_px,
                            uint32_t modifiers)
{
  if (xid == 0) return;
  x11_proto_bridge_post_enter(xid, x_px, y_px, modifiers);
}


void x11_post_pointer_leave(uint32_t xid,
                            int32_t x_px,
                            int32_t y_px,
                            uint32_t modifiers)
{
  if (xid == 0) return;
  x11_proto_bridge_post_leave(xid, x_px, y_px, modifiers);
}


void x11_post_window_raise(uint32_t xid)
{  
    x11_ui_push_raise(xid);
}


void x11_post_window_destroy(uint32_t xid)
{
  x11_ui_push_destroy(xid);
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

  x11_ui_push_map(xid);
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

  x11_ui_push_unmap(xid);
}


void x11_post_window_resize(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (xid == 0) return;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] post_window_resize: xid=0x%08X new=%dx%d (pre-gate)\n",
          (unsigned)xid, (int)w_px, (int)h_px);
#endif
  
  // Gate on existence (and optionally "not closing") to avoid stale events.
  x11_backend_lock();
  const int exists = x11_backend_window_exists_locked(xid);
  const int closing = exists ? x11_backend_window_is_closing_locked(xid) : 0;
  x11_backend_unlock();

  if (!exists || closing) {
#ifndef NDEBUG
    fprintf(stderr,
            "[SwiftX11] post_window_resize: DROP xid=0x%08X exists=%d closing=%d\n",
            (unsigned)xid, exists, closing);
#endif
    return;
  }
  
  // Update backend truth first.
  x11_backend_window_set_size(xid, w_px, h_px);
    
  // Tell xproto/server-thread to resize its window+fb too.
  int ok = x11_requests_push_rootless_resize(xid, w_px, h_px);
#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] post_window_resize: PUSH_ROOTLESS xid=0x%08X %dx%d ok=%d\n",
        (unsigned)xid, (int)w_px, (int)h_px, ok);
#endif
  
  
  x11_ui_push_resize(xid, w_px, h_px);
  // Wake repaint loop so the resize shows immediately.
  x11_server_wakeup();
}


// Server-driven configure: update backend size + wake UI.
// IMPORTANT: does NOT enqueue ROOTLESS_RESIZE back to server.
void x11_apply_window_configure(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (xid == 0) return;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  x11_backend_lock();
  const int exists = x11_backend_window_exists_locked(xid);
  const int closing = exists ? x11_backend_window_is_closing_locked(xid) : 0;
  x11_backend_unlock();
  if (!exists || closing) return;

  x11_backend_window_set_size(xid, w_px, h_px);

  // Optional: emit a debug event if you want to track configures separately
  // (NOT EV_WINDOW_RESIZE unless you really want it).
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

// -----------------------------------------------------------------------------
// Damage request side-effect
//
// This is called from the server-thread request drain (x11_requests_drain_on_server_thread)
// when an X11_REQ_DAMAGE is dequeued.
//
// IMPORTANT: do NOT push another request here (that would create a request-loop).
// Instead, mark backend damage and wake the repaint runloop.
// -----------------------------------------------------------------------------
void x11_server_emit_window_damage(uint32_t xid)
{
  if (xid == 0) return;

  // Gate on existence and "not closing" so stale damage doesn't resurrect windows.
  int ok = 0;
  int exists = 0;
  int closing = 0;
  x11_backend_lock();
  exists = x11_backend_window_exists_locked(xid);
  closing = exists ? x11_backend_window_is_closing_locked(xid) : 0;
  if (exists && !closing) {
    x11_backend_mark_damage_locked(xid);
    ok = 1;
  }
  x11_backend_unlock();

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] x11_server_emit_window_damage: xid=0x%08X exists=%d closing=%d ok=%d\n",
          (unsigned)xid, exists, closing, ok);
#endif

  if (!ok) return;
  
  // Backend event: DAMAGE (distinct from request queue)
  
  x11_ui_push_damage(xid, 0, 0, 0, 0);
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
  x11_requests_drain_on_server_thread();
  
  // 1) Then snapshot damaged windows and repaint them.
  uint32_t xids[X11_MAX_WINDOWS];
  int32_t  ws[X11_MAX_WINDOWS];
  int32_t  hs[X11_MAX_WINDOWS];
  int n = 0;

  // Ask backend for the list of damaged windows (also clears damage internally)
  n = x11_backend_take_damaged_snapshot(xids, ws, hs, X11_MAX_WINDOWS);

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


uint32_t x11_window_create(const char* title, int32_t w_px, int32_t h_px) {
#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] ERROR: x11_window_create() should not be called (event-driven path)\n");
  abort();
#endif
}


void x11_window_set_title(uint32_t xid, const char* title_utf8)
{
  if (!title_utf8) title_utf8 = "";

  // Optional: drop if window is gone (keeps queue cleaner).
  x11_backend_lock();
  int exists = x11_backend_window_exists_locked(xid);
  x11_backend_unlock();
  if (!exists) return;

  x11_ui_push_title(xid, title_utf8);
  // Wake so the UI sees it promptly.
  x11_server_wakeup();
}

// x11_shim.c (non-static wrappers)
void x11_server_emit_window_create(uint32_t xid, uint32_t parent_xid, const char* title, int32_t w, int32_t h)
{
  x11_emit_window_create(xid, parent_xid, title, w, h);
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
  
  x11_ui_push_map(xid);
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
  
  x11_ui_push_unmap(xid);
}


void x11_server_apply_configure_request(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (xid == 0) return;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] apply_configure_request: xid=0x%08X size=%dx%d\n",
          (unsigned)xid, (int)w_px, (int)h_px);
#endif
  
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

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] apply_configure_request: xid=0x%08X did=1 mapped=%d -> resize event\n",
          (unsigned)xid, mapped);
#endif
  
  x11_ui_push_resize(xid, w_px, h_px);
  if (mapped) x11_server_wakeup();
}


int x11_server_copy_window_bgra(uint32_t xid,
                                uint8_t* out_bytes,
                                int32_t out_cap,
                                int32_t* out_w,
                                int32_t* out_h,
                                int32_t* out_bpr)
{
#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] shim: x11_server_copy_window_bgra xid=0x%08X out_bytes=%p cap=%d\n",
          (unsigned)xid, (void*)out_bytes, (int)out_cap);
#endif

  return x11_xproto_copy_window_bgra(xid, out_bytes, out_cap, out_w, out_h, out_bpr);
}

void x11_post_window_presentable(uint32_t xid)
{
  if (xid == 0) return;

  x11_backend_lock();
  // If you track this state in backend too, set it there.
  // But at minimum, tell xproto.
  x11_backend_unlock();

  // Push a request to server thread so xproto mutation happens on the server thread.
  x11_requests_push_window_presentable(xid);
  x11_server_wakeup();
}


// Internal C ABI implemented in C++.
void x11_proto_bridge_apply_rootless_resize(uint32_t wid, int32_t w_px, int32_t h_px);
void x11_xproto_apply_rootless_resize_on_server_thread(uint32_t wid, int32_t w_px, int32_t h_px)
{
  x11_proto_bridge_apply_rootless_resize(wid, w_px, h_px);
}


