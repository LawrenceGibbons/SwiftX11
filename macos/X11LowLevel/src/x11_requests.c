//
//  x11_requests.c
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/6/26.
//  This file models client → server requests (X11 protocol–like).
//

#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

#include "x11_requests.h"
#include "x11_shim.h"
#include "x11_backend.h"
#include "x11_server_internal.h"
#include "x11_xproto.h"
#include "XProtoServerBridge.h"

// ---------------- Client request queue (C -> server thread) ------------------

typedef enum {
  X11_REQ_NONE = 0,
  X11_REQ_CREATE,
  X11_REQ_DESTROY,
  X11_REQ_MAP,
  X11_REQ_UNMAP,
  X11_REQ_CONFIGURE,   // resize for now
  X11_REQ_SET_TITLE,
  X11_REQ_DAMAGE,
  X11_REQ_ROOTLESS_RESIZE,
  X11_REQ_WINDOW_PRESENTABLE,
} x11_client_req_type_t;

typedef struct {
  x11_client_req_type_t type;
  uint32_t xid;
  uint32_t parent_xid;

  // Optional UTF-8 payload used by CREATE and SET_TITLE.
  // Always NUL-terminated.
  char    title[X11_TEXT_MAX];
  uint8_t title_len;

  union {
    struct { int32_t w_px, h_px; } create;
    struct { int32_t w_px, h_px; } configure;
    struct { int32_t w_px, h_px; } rootless_resize;
  } u;
} x11_client_req_t;

#ifndef X11_CLIENT_REQ_CAP
#define X11_CLIENT_REQ_CAP 1024
#endif

static x11_client_req_t g_req_q[X11_CLIENT_REQ_CAP];
static uint32_t g_req_r = 0;
static uint32_t g_req_w = 0;

static inline uint32_t req_next(uint32_t v) { return (v + 1u) % X11_CLIENT_REQ_CAP; }
static inline uint32_t req_prev(uint32_t v) { return (v + X11_CLIENT_REQ_CAP - 1u) % X11_CLIENT_REQ_CAP; }
static inline int req_is_empty(void) { return (g_req_r == g_req_w); }





static int req_push_locked(const x11_client_req_t *req)
{
  if (!req) return 0;

  // Coalesce ONLY when the last queued request is the same xid + same type,
  // so we don't reorder around MAP/UNMAP/DESTROY etc.
  if (!req_is_empty()) {
    const uint32_t prev_i = req_prev(g_req_w);
    x11_client_req_t *prev = &g_req_q[prev_i];

    if (prev->xid == req->xid) {
      // Coalesce CONFIGURE (resize)
      if (prev->type == X11_REQ_CONFIGURE && req->type == X11_REQ_CONFIGURE) {
        prev->u.configure = req->u.configure;
        return 1;
      }

      // Coalesce ROOTLESS_RESIZE (host-driven resize)
      if (prev->type == X11_REQ_ROOTLESS_RESIZE && req->type == X11_REQ_ROOTLESS_RESIZE) {
        prev->u.rootless_resize = req->u.rootless_resize;
        return 1;
      }

      // Coalesce SET_TITLE
      if (prev->type == X11_REQ_SET_TITLE && req->type == X11_REQ_SET_TITLE) {
        prev->title_len = req->title_len;
        memcpy(prev->title, req->title, X11_TEXT_MAX);
        return 1;
      }

      // Optional (safe) idempotent drops:
      // MAP after MAP for same xid -> drop
      if (prev->type == X11_REQ_MAP && req->type == X11_REQ_MAP) {
        return 1;
      }
      // UNMAP after UNMAP for same xid -> drop
      if (prev->type == X11_REQ_UNMAP && req->type == X11_REQ_UNMAP) {
        return 1;
      }
      // DAMAGE after DAMAGE for same xid -> drop (safe coalesce)
      if (prev->type == X11_REQ_DAMAGE && req->type == X11_REQ_DAMAGE) {
        return 1;
      }
    }
  }

  // Normal enqueue path
  uint32_t next = req_next(g_req_w);
  if (next == g_req_r) {
    // full -> drop (or overwrite oldest if you prefer)
    return 0;
  }

  g_req_q[g_req_w] = *req;
  g_req_w = next;
  return 1;
}


static int req_pop_locked(x11_client_req_t *out) {
  if (g_req_r == g_req_w) return 0;
  *out = g_req_q[g_req_r];
  g_req_r = req_next(g_req_r);
  return 1;
}


static _Atomic uint32_t g_next_xid = 0x20000; // pick a range away from test ids

static uint32_t alloc_xid(void) {
  // simple monotonic; wrap to nonzero if needed
  uint32_t xid = atomic_fetch_add_explicit(&g_next_xid, 1, memory_order_relaxed);
  if (xid == 0) xid = atomic_fetch_add_explicit(&g_next_xid, 1, memory_order_relaxed);
  return xid;
}

uint32_t x11_client_create_window(const char* title_utf8, int32_t w_px, int32_t h_px)
{
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  x11_client_req_t r = {0};
  r.type = X11_REQ_CREATE;
  r.xid  = alloc_xid();
  r.u.create.w_px = w_px;
  r.u.create.h_px = h_px;

  // Copy title into fixed buffer (truncate safely)
  if (title_utf8) {
    size_t n = strnlen(title_utf8, X11_TEXT_MAX-1);
    memcpy(r.title, title_utf8, n);
    r.title_len = (uint8_t)n;
    if (n < X11_TEXT_MAX) r.title[n] = 0;
    else r.title[X11_TEXT_MAX - 1] = 0;
  } else {
    r.title_len = 0;
    r.title[0] = 0;
  }

  x11_backend_lock();
  (void)req_push_locked(&r);
  x11_backend_unlock();

  x11_server_wakeup();
  return r.xid;
}

void x11_client_destroy_window(uint32_t xid)
{
  if (xid == 0) return;
  x11_client_req_t r = {0};
  r.type = X11_REQ_DESTROY;
  r.xid = xid;

  x11_backend_lock();
  (void)req_push_locked(&r);
  x11_backend_unlock();
  x11_server_wakeup();
}

void x11_client_destroy_window_async(uint32_t xid)
{
  // You can keep the async helper if you want, but for “client req queue”
  // it's already async from UI perspective. So just alias:
  x11_client_destroy_window(xid);
}

void x11_client_map_window(uint32_t xid)
{
  if (xid == 0) return;
  x11_client_req_t r = {0};
  r.type = X11_REQ_MAP;
  r.xid = xid;

  x11_backend_lock();
  (void)req_push_locked(&r);
  x11_backend_unlock();
  x11_server_wakeup();
}

void x11_client_unmap_window(uint32_t xid)
{
  if (xid == 0) return;
  x11_client_req_t r = {0};
  r.type = X11_REQ_UNMAP;
  r.xid = xid;

  x11_backend_lock();
  (void)req_push_locked(&r);
  x11_backend_unlock();
  x11_server_wakeup();
}

void x11_client_configure_window(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (xid == 0) return;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  x11_client_req_t r = {0};
  r.type = X11_REQ_CONFIGURE;
  r.xid = xid;
  r.u.configure.w_px = w_px;
  r.u.configure.h_px = h_px;

  x11_backend_lock();
  (void)req_push_locked(&r);
  x11_backend_unlock();
  x11_server_wakeup();
}

void x11_client_set_window_title(uint32_t xid, const char* title_utf8)
{
  if (xid == 0) return;
  x11_client_req_t r = {0};
  r.type = X11_REQ_SET_TITLE;
  r.xid  = xid;

  if (title_utf8) {
    size_t n = strnlen(title_utf8, X11_TEXT_MAX-1);
    memcpy(r.title, title_utf8, n);
    r.title_len = (uint8_t)n;
    if (n < X11_TEXT_MAX) r.title[n] = 0;
    else r.title[X11_TEXT_MAX - 1] = 0;
  } else {
    r.title_len = 0;
    r.title[0] = 0;
  }

  x11_backend_lock();
  (void)req_push_locked(&r);
  x11_backend_unlock();
  x11_server_wakeup();
}

// ---- Server-queue push APIs (used by x11_xproto thread)
// These enqueue onto the same client->server request queue.
// Return 1 on success, 0 if dropped.

int x11_requests_push_create(uint32_t xid, uint32_t parent_xid, const char* title_utf8, int32_t w_px, int32_t h_px)
{
  if (xid == 0) return 0;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  x11_client_req_t r = {0};
  r.type = X11_REQ_CREATE;
  r.xid  = xid;
  r.parent_xid = parent_xid;
  r.u.create.w_px = w_px;
  r.u.create.h_px = h_px;


  // Copy title into fixed buffer (truncate safely)
  if (title_utf8) {
    size_t n = strnlen(title_utf8, X11_TEXT_MAX - 1);
    memcpy(r.title, title_utf8, n);
    r.title_len = (uint8_t)n;
    r.title[n] = 0;
  } else {
    r.title_len = 0;
    r.title[0] = 0;
  }

  x11_backend_lock();
  int ok = req_push_locked(&r);
  x11_backend_unlock();

  if (ok) x11_server_wakeup();
  return ok;
}

int x11_requests_push_destroy(uint32_t xid)
{
  if (xid == 0) return 0;

  x11_client_req_t r = {0};
  r.type = X11_REQ_DESTROY;
  r.xid  = xid;

  x11_backend_lock();
  int ok = req_push_locked(&r);
  x11_backend_unlock();

  if (ok) x11_server_wakeup();
  return ok;
}

int x11_requests_push_map(uint32_t xid)
{
  if (xid == 0) return 0;

  x11_client_req_t r = {0};
  r.type = X11_REQ_MAP;
  r.xid  = xid;

  x11_backend_lock();
  int ok = req_push_locked(&r);
  x11_backend_unlock();

  if (ok) x11_server_wakeup();
  return ok;
}

int x11_requests_push_window_presentable(uint32_t xid)
{
  if (xid == 0) return 0;

  x11_client_req_t r = {0};
  r.type = X11_REQ_WINDOW_PRESENTABLE;
  r.xid  = xid;

  x11_backend_lock();
  int ok = req_push_locked(&r);
  x11_backend_unlock();

  if (ok) x11_server_wakeup();
  return ok;
}

int x11_requests_push_unmap(uint32_t xid)
{
  if (xid == 0) return 0;

  x11_client_req_t r = {0};
  r.type = X11_REQ_UNMAP;
  r.xid  = xid;

  x11_backend_lock();
  int ok = req_push_locked(&r);
  x11_backend_unlock();

  if (ok) x11_server_wakeup();
  return ok;
}

int x11_requests_push_configure(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (xid == 0) return 0;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  x11_client_req_t r = {0};
  r.type = X11_REQ_CONFIGURE;
  r.xid  = xid;
  r.u.configure.w_px = w_px;
  r.u.configure.h_px = h_px;

  x11_backend_lock();
  int ok = req_push_locked(&r);
  x11_backend_unlock();

  if (ok) x11_server_wakeup();
  return ok;
}


int x11_requests_push_rootless_resize(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (xid == 0) return 0;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  
  x11_client_req_t r = {0};
  r.type = X11_REQ_ROOTLESS_RESIZE;
  r.xid = xid;
  r.u.rootless_resize.w_px = w_px;
  r.u.rootless_resize.h_px = h_px;
  
  x11_backend_lock();
  int ok = req_push_locked(&r);
  x11_backend_unlock();

  fprintf(stderr, "[SwiftX11] push_rootless_resize: enqueue type=%d xid=0x%08X %dx%d\n",
          (int)X11_REQ_ROOTLESS_RESIZE, (unsigned)xid, (int)w_px, (int)h_px);
#ifndef NDEBUG
fprintf(stderr, "[SwiftX11] push_rootless_resize: xid=0x%08X %dx%d ok=%d\n",
        (unsigned)xid, (int)w_px, (int)h_px, ok);
#endif
  
  return ok;
}


int x11_requests_push_set_title(uint32_t xid, const char* title_utf8)
{
  if (xid == 0) return 0;

  x11_client_req_t r = {0};
  r.type = X11_REQ_SET_TITLE;
  r.xid  = xid;

  if (title_utf8) {
    size_t n = strnlen(title_utf8, X11_TEXT_MAX - 1);
    memcpy(r.title, title_utf8, n);
    r.title_len = (uint8_t)n;
    r.title[n] = 0;
  } else {
    r.title_len = 0;
    r.title[0] = 0;
  }

  x11_backend_lock();
  int ok = req_push_locked(&r);
  x11_backend_unlock();

  if (ok) x11_server_wakeup();
  return ok;
}

int x11_requests_push_damage(uint32_t xid)
{
  if (xid == 0) return 0;
  x11_client_req_t r = {0};
  r.type = X11_REQ_DAMAGE;
  r.xid  = xid;
  x11_backend_lock();
  int ok = req_push_locked(&r);
  x11_backend_unlock();

#ifndef NDEBUG
  // ok==0 means the queue was full and we dropped the request.
  // ok==1 means enqueued OR coalesced (req_push_locked coalesces adjacent DAMAGE for same xid).
  fprintf(stderr, "[SwiftX11] x11_requests_push_damage: xid=0x%08X ok=%d\n",
          (unsigned)xid, ok);
#endif

  if (ok) x11_server_wakeup();
  return ok;
}

// Called by the server/runloop thread (e.g. from x11_server_step).
void x11_requests_drain_on_server_thread(void)
{
  for (;;) {
    x11_client_req_t r;
    x11_backend_lock();
    int ok = req_pop_locked(&r);
    x11_backend_unlock();
    if (!ok) break;

#ifndef NDEBUG
    fprintf(stderr, "[SwiftX11] drain_on_server_thread: POP REQ type=%d xid=0x%08X\n",
            r.type, (unsigned)r.xid);
#endif
    
    switch (r.type) {
      case X11_REQ_CREATE: {
        // Create the backend slot + Swift window via existing helper.
        // IMPORTANT: use r.xid (already allocated), and title from r.title.
        const char* title = (r.title_len > 0) ? r.title : "SwiftX11 Window";
        x11_server_emit_window_create(r.xid, r.parent_xid, title, r.u.create.w_px, r.u.create.h_px);

        // X11-ish default: created != mapped. So DO NOT map here.
        // Your Swift side creates hidden/unmapped now — great.
      } break;

      case X11_REQ_DESTROY:
        x11_server_emit_window_destroy(r.xid);
        break;

      case X11_REQ_MAP:
        x11_post_window_map(r.xid);
        break;

      case X11_REQ_UNMAP:
        x11_post_window_unmap(r.xid);
        break;

      case X11_REQ_CONFIGURE:
        x11_apply_window_configure(r.xid, r.u.configure.w_px, r.u.configure.h_px);
        break;

      case X11_REQ_SET_TITLE:
        x11_window_set_title(r.xid, r.title);
        break;

      case X11_REQ_DAMAGE:
#ifndef NDEBUG
        fprintf(stderr,
                "[SwiftX11] drain_on_server_thread: POP DAMAGE xid=0x%08X -> emit_window_damage\n",
                (unsigned)r.xid);
#endif
        x11_server_emit_window_damage(r.xid); // implement in shim
        break;
        
        
        
      case X11_REQ_ROOTLESS_RESIZE: {
#ifndef NDEBUG
        fprintf(stderr, "[SwiftX11] drain_on_server_thread: APPLY ROOTLESS_RESIZE xid=0x%08X %dx%d\n",
                (unsigned)r.xid, (int)r.u.rootless_resize.w_px, (int)r.u.rootless_resize.h_px);
#endif
        // Runs on server thread. Do not take backend lock here if your xproto state
        // (win_find/g_framebuffers) is not protected by it.
        x11_xproto_apply_rootless_resize_on_server_thread(r.xid,
                                                          r.u.rootless_resize.w_px,
                                                          r.u.rootless_resize.h_px);
        break;
      }
        
      case X11_REQ_WINDOW_PRESENTABLE:
        x11_proto_bridge_window_set_presentable_and_flush(r.xid);
        break;
        
      default:
        break;
    }
  }
}
