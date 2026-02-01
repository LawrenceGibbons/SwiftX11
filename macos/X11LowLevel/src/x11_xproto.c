//
//  x11_xproto.c
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/7/26.
//

#include "x11_xproto.h"

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>   // atan2f, fmodf

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "x11_requests.h"
#include "XProtoConnectionWrapper.h"
#include "XProtoServerBridge.h"
#include "XProtoGCBridge.hpp"

static void enqueue_destroy_window(uint32_t xid)
{
  (void)x11_requests_push_destroy(xid);
}

static void enqueue_map_window(uint32_t xid)
{
  (void)x11_requests_push_map(xid);
}

static void enqueue_configure_window(uint32_t xid, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
  (void)x; (void)y;
  (void)x11_requests_push_configure(xid, (int32_t)w, (int32_t)h);
}

// For SIGPIPE avoidance on macOS
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// -----------------------------------------------------------------------------
// Debug tracing toggles
//
// Keep protocol-correctness checks under !NDEBUG, but hide noisy fprintf tracing
// behind explicit compile-time flags so DEBUG builds stay usable.
//
// Set these to 1 in your build settings when you want extra logs.
// -----------------------------------------------------------------------------
#ifndef SWIFTX11_TRACE
#define SWIFTX11_TRACE 0
#endif

#ifndef SWIFTX11_TRACE_NOOP_DRAW
#define SWIFTX11_TRACE_NOOP_DRAW 0
#endif

#ifndef SWIFTX11_TRACE_DUMP_70_71
#define SWIFTX11_TRACE_DUMP_70_71 0
#endif


// -----------------------------------------------------------------------------
// X11 socket + protocol scaffold
//
// Goal (Step 1): accept TCP connections on localhost:6000+display and respond
// with a well-formed X11 SetupFailed reply.
//
// This deliberately does NOT implement the X11 protocol yet; it just wires the
// transport + a minimal handshake so we can prove clients (xeyes/xterm/etc.) can
// reach us.
// -----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// Module state (no dependency on g_srv)
// ----------------------------------------------------------------------------
#ifndef X11_MAX_PIXMAPS
#define X11_MAX_PIXMAPS 256
#endif

typedef struct {
  uint32_t xid;
  uint32_t width;
  uint32_t height;
  uint8_t  depth;

  // depth==1: packed bitmap bits, scanlines padded to 32 bits
  uint32_t stride_bytes;

  // depth!=1: 32-bit pixels
  uint32_t *pixels;

  // depth==1: packed bits
  uint8_t  *bits;
} x11_pixmap_t;


static _Atomic int g_stop = 0;
static _Atomic int g_running = 0;
static int g_lfd = -1;
static pthread_t g_thread;
static int g_current_client_fd = -1;

typedef struct {
  uint32_t xid;
  uint32_t parent;
  int16_t  x;
  int16_t  y;
  uint16_t w;
  uint16_t h;
  uint8_t  mapped;      // 0/10
  uint8_t  dirty;       // 1 if drawing happened while unmapped
  uint8_t presentable;   // Cocoa says safe to present
  uint32_t event_mask;  // from ChangeWindowAttributes / CreateWindow
  bool pending_damage;
  uint16_t _pad0;
  int owner_fd;   // client socket that created this window
} x11_win_t;

// Per-window framebuffer
typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t* pixels; // ARGB8888 buffer
} x11_fb_t;


static x11_win_t g_wins[256];
static x11_fb_t g_framebuffers[256]; // parallel to g_wins
static size_t g_wins_n = 0;

typedef struct {
  uint32_t wid;
  uint32_t atom;
  uint32_t type;
  uint8_t  format;   // 8/16/32
  uint8_t  _pad0[3];
  uint32_t nbytes;
  uint8_t* data;
} x11_prop_t;

static x11_prop_t g_props[512];
static size_t g_props_n = 0;

static uint32_t rd32(const uint8_t* p){ return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)); }

static void wr16_le(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr32_le(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void prop_delete_all_for_window(uint32_t wid)
{
  size_t i = 0;
  while (i < g_props_n) {
    if (g_props[i].wid == wid) {
      free(g_props[i].data);
      g_props[i] = g_props[g_props_n - 1];
      g_props_n--;
      continue; // re-check swapped entry
    }
    i++;
  }
}


// For SIGPIPE avoidance on macOS
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif


// Returns:
//   1  = success (read exactly n bytes)
//   0  = peer closed (EOF) before full read
//  -2  = timeout occurred before full read (caller may retry)
//  -1  = fatal socket error
static int x11_recv_exact(int fd, uint8_t *dst, size_t n)
{
  size_t off = 0;

  while (off < n) {
    ssize_t r = recv(fd, dst + off, n - off, 0);
    if (r > 0) {
      off += (size_t)r;
      continue;
    }
    if (r == 0) {
      return 0; // EOF
    }

    // r < 0
    if (errno == EINTR) continue;

    // Because you set SO_RCVTIMEO, timeouts come back as EAGAIN/EWOULDBLOCK.
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (atomic_load_explicit(&g_stop, memory_order_relaxed)) return -2;
      continue; // keep trying until we get all bytes
    }
    return -1;
  }

  return 1;
}




static ssize_t win_index(uint32_t xid)
{
  for (size_t i = 0; i < g_wins_n; i++) {
    if (g_wins[i].xid == xid) return (ssize_t)i;
  }
  return -1;
}

static x11_win_t* win_find(uint32_t xid)
{
  const ssize_t idx = win_index(xid);
  return (idx >= 0) ? &g_wins[(size_t)idx] : NULL;
}

// Add a new window slot and keep g_framebuffers[] aligned.
// Returns the index on success, -1 on failure.
static ssize_t win_add(uint32_t xid)
{
  if (g_wins_n >= (sizeof(g_wins)/sizeof(g_wins[0]))) return -1;

  const size_t idx = g_wins_n++;
  x11_win_t* w = &g_wins[idx];
  memset(w, 0, sizeof(*w));
  w->xid = xid;

  // IMPORTANT: keep framebuffer slot in a known state.
  x11_fb_t* fb = &g_framebuffers[idx];
  memset(fb, 0, sizeof(*fb));

  return (ssize_t)idx;
}


#ifndef NDEBUG
static void dbg_dump_windows_brief(const char* tag, uint32_t target_xid) {
  fprintf(stderr, "[SwiftX11][SNAP] %s: target=0x%08X g_wins_n=%zu\n",
          tag, (unsigned)target_xid, g_wins_n);

  // Dump up to first 64 windows for sanity.
  size_t limit = g_wins_n < 64 ? g_wins_n : 64;
  for (size_t i = 0; i < limit; i++) {
    const x11_win_t* w = &g_wins[i];
    if (!w->xid) continue;

    fprintf(stderr,
            "[SwiftX11][SNAP]   i=%zu xid=0x%08X parent=0x%08X xy=(%d,%d) wh=%ux%u mapped=%u presentable=%u dirty=%u owner_fd=%d mask=0x%08X\n",
            i,
            (unsigned)w->xid,
            (unsigned)w->parent,
            (int)w->x, (int)w->y,
            (unsigned)w->w, (unsigned)w->h,
            (unsigned)w->mapped,
            (unsigned)w->presentable,
            (unsigned)w->dirty,
            (int)w->owner_fd,
            (unsigned)w->event_mask);
  }
}
#endif


int x11_xproto_snapshot_window_view(uint32_t xid,
                                   int16_t* out_x,
                                   int16_t* out_y,
                                   uint16_t* out_w,
                                   uint16_t* out_h,
                                   uint32_t* out_event_mask,
                                   int* out_mapped,
                                   int* out_owner_fd)
{
#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11][SNAP] ENTER xid=0x%08X\n", (unsigned)xid);
#endif
  const x11_win_t* found = NULL;
  size_t found_i = (size_t)-1;

  // Scan g_wins[] linearly (don’t call win_find yet; we want to verify it)
  for (size_t i = 0; i < g_wins_n; i++) {
    const x11_win_t* w = &g_wins[i];
    if (w->xid == xid) {
      found = w;
      found_i = i;
      break;
    }
  }

  if (!found) {
#ifndef NDEBUG
    fprintf(stderr, "[SwiftX11][SNAP] NOT FOUND xid=0x%08X\n", (unsigned)xid);
    dbg_dump_windows_brief("NOT_FOUND_DUMP", xid);

    // Also try to find “neighbors”: host/child patterns
    // (This is helpful if you know host is xid-1 or parent is 0x1000000A, etc.)
#endif
    return 0;
  }

#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11][SNAP] FOUND xid=0x%08X at i=%zu parent=0x%08X owner_fd=%d mapped=%u presentable=%u dirty=%u mask=0x%08X wh=%ux%u\n",
          (unsigned)xid,
          found_i,
          (unsigned)found->parent,
          (int)found->owner_fd,
          (unsigned)found->mapped,
          (unsigned)found->presentable,
          (unsigned)found->dirty,
          (unsigned)found->event_mask,
          (unsigned)found->w,
          (unsigned)found->h);
#endif

  // Fill outputs (use found-> fields)
  if (out_x) *out_x = found->x;
  if (out_y) *out_y = found->y;
  if (out_w) *out_w = found->w;
  if (out_h) *out_h = found->h;
  if (out_event_mask) *out_event_mask = found->event_mask;
  if (out_mapped) *out_mapped = found->mapped ? 1 : 0;
  if (out_owner_fd) *out_owner_fd = found->owner_fd;

//  x11_win_t* w = win_find(xid);
//  if (!w) {    
//    return 0;
//  }
//  
//  if (out_x) *out_x = w->x;
//  if (out_y) *out_y = w->y;
//  if (out_w) *out_w = w->w;
//  if (out_h) *out_h = w->h;
//  if (out_event_mask) *out_event_mask = w->event_mask;
//  if (out_mapped) *out_mapped = (int)w->mapped;
//  if (out_owner_fd) *out_owner_fd = w->owner_fd;
  return 1;
}

int x11_xproto_copy_window_bgra(uint32_t xid,
                                uint8_t* out_bytes,
                                int32_t out_cap,
                                int32_t* out_w,
                                int32_t* out_h,
                                int32_t* out_bpr)
{
#ifndef NDEBUG
fprintf(stderr, "[SwiftX11] xproto: copy_window_bgra ENTER xid=0x%08X out_bytes=%p cap=%d\n",
        (unsigned)xid, (void*)out_bytes, (int)out_cap);
  fprintf(stderr,
          "[SwiftX11] xproto_copy_window_bgra: ************************************************************************* gets called\n");
#endif

  const ssize_t idx = win_index(xid);
  x11_fb_t* fb = (idx >= 0) ? &g_framebuffers[(size_t)idx] : NULL;

  if (!fb || !fb->pixels || fb->width == 0 || fb->height == 0) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_bpr) *out_bpr = 0;
    return 0;
  }

  int32_t w = (int32_t)fb->width;
  int32_t h = (int32_t)fb->height;
  int32_t bpr = w * 4;

  int64_t needed64 = (int64_t)bpr * (int64_t)h;
  if (needed64 <= 0 || needed64 > INT32_MAX) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_bpr) *out_bpr = 0;
    return 0;
  }
  const int32_t needed = (int32_t)needed64;

  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  if (out_bpr) *out_bpr = bpr;

  // Allow a size-only query: caller passes out_bytes==NULL and/or out_cap==0.
  // In that case, we return success after reporting w/h/bpr.
  if (!out_bytes || out_cap == 0) return 1;

  // If the caller provided a buffer, it must be large enough.
  if (out_cap < needed) return 0;

#ifndef NDEBUG
  {
    // Sample a few pixels so we can confirm the xproto FB is what the UI copies.
    const int32_t sx_c = w / 2;
    const int32_t sy_c = h / 2;
    const int32_t sx_l = w / 4;
    const int32_t sx_r = (w * 3) / 4;
    const int32_t sy_m = h / 2;

    uint32_t sp_c = 0, sp_l = 0, sp_r = 0;
    if (sx_c >= 0 && sy_c >= 0 && sx_c < w && sy_c < h) {
      sp_c = fb->pixels[(size_t)sy_c * (size_t)fb->width + (size_t)sx_c];
    }
    if (sx_l >= 0 && sy_m >= 0 && sx_l < w && sy_m < h) {
      sp_l = fb->pixels[(size_t)sy_m * (size_t)fb->width + (size_t)sx_l];
    }
    if (sx_r >= 0 && sy_m >= 0 && sx_r < w && sy_m < h) {
      sp_r = fb->pixels[(size_t)sy_m * (size_t)fb->width + (size_t)sx_r];
    }

    // Count non-white pixels (cheap sanity check for "did we draw anything?")
    size_t nonwhite = 0;
    const size_t npx = (size_t)fb->width * (size_t)fb->height;
    for (size_t i = 0; i < npx; i++) {
      if (fb->pixels[i] != 0xFFFFFFFFu) nonwhite++;
    }

    fprintf(stderr,
            "[SwiftX11] xproto_copy_window_bgra: xid=0x%08X fb=%p %dx%d nonwhite=%zu sample L/C/R=0x%08X 0x%08X 0x%08X\n",
            (unsigned)xid, (void*)fb->pixels, (int)w, (int)h,
            nonwhite, (unsigned)sp_l, (unsigned)sp_c, (unsigned)sp_r);
  }
#endif

  memcpy(out_bytes, (const void*)fb->pixels, (size_t)needed);
  return 1;
}


static void enqueue_damage_window(uint32_t xid)
{
  // Defer damage while not ready (unmapped or not presentable).
  if (!x11_proto_bridge_window_is_ready_to_present(xid)) {
    x11_proto_bridge_window_mark_dirty(xid);
    return;
  }
  
  int ok = x11_requests_push_damage(xid);
}

// Record the xproto client thread so other threads can detect "not on xproto thread".
static pthread_t g_xproto_thread;
static int g_xproto_thread_valid = 0;

#ifndef NDEBUG
// expose setters if you want, or wire to your existing globals instead
void dbg_set_xproto_thread(pthread_t tid) { g_xproto_thread = tid; g_xproto_thread_valid = 1; }
void dbg_clear_xproto_thread(void) { g_xproto_thread_valid = 0; }



void dbg_require_xproto_thread(const char* what)
{
  if (!g_xproto_thread_valid) return;
  if (!pthread_equal(pthread_self(), g_xproto_thread)) {
    fprintf(stderr,
            "[SwiftX11] FATAL: socket write from non-xproto thread in %s (self=%p xproto=%p)\n",
            what,
            (void*)pthread_self(),
            (void*)g_xproto_thread);
    fflush(stderr);
    abort();
  }
}
#endif


static int x11_send_all_fd(int fd, const void* buf, size_t n)
{
#ifndef NDEBUG
  dbg_require_xproto_thread("x11_send_all");
#endif

  const uint8_t* p = (const uint8_t*)buf;
  while (n) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      return 0;
    }
    if (w == 0) return 0;
    p += (size_t)w;
    n -= (size_t)w;
  }
  return 1;
}

#include "XProtoWindowCBridge.h" // add near other includes

int x11_xproto_c_create_window_slot(uint32_t wid,
                                   uint32_t parent,
                                   int16_t x,
                                   int16_t y,
                                   uint16_t wpx,
                                   uint16_t hpx,
                                   uint32_t event_mask,
                                   int owner_fd,
                                   int* out_dirty)
{
  if (out_dirty) *out_dirty = 0;
  if (wid == 0) return 0;

  // Create or overwrite (idempotent-ish), keep alignment g_wins[] <-> g_framebuffers[].
  ssize_t idx = win_index(wid);
  if (idx < 0) idx = win_add(wid);
  if (idx < 0) return 0;

  x11_win_t* w = &g_wins[(size_t)idx];

  // Mirror exactly what handle_CreateWindow used to do
  w->parent      = parent;
  w->x           = x;
  w->y           = y;
  w->w           = (wpx ? wpx : 1);
  w->h           = (hpx ? hpx : 1);
  w->mapped      = 0;
  w->dirty       = 0;
  w->event_mask  = event_mask;
  w->presentable = 0;
  w->owner_fd    = owner_fd;

  // Initialize/re-init framebuffer at SAME index.
  x11_fb_t* fb = &g_framebuffers[(size_t)idx];

  if (fb->pixels) {
    free(fb->pixels);
    fb->pixels = NULL;
  }

  fb->width  = (uint32_t)w->w;
  fb->height = (uint32_t)w->h;

  fb->pixels = (uint32_t*)malloc((size_t)fb->width * (size_t)fb->height * sizeof(uint32_t));
  if (fb->pixels) {
    const size_t npx = (size_t)fb->width * (size_t)fb->height;
    for (size_t i = 0; i < npx; i++) fb->pixels[i] = 0xFFFFFFFFu;
    w->dirty = 1;
    if (out_dirty) *out_dirty = 1;
  } else {
    fb->width = 0;
    fb->height = 0;
    return 0; // safest: fail hard if FB alloc fails
  }

  return 1;
}

// Writable window FB pointer + dimensions
int x11_xproto_window_fb_rw(uint32_t xid,
                            uint32_t** outPixels,
                            uint32_t* outW,
                            uint32_t* outH)
{
  if (outPixels) *outPixels = NULL;
  if (outW) *outW = 0;
  if (outH) *outH = 0;

  const ssize_t idx = win_index(xid);
  if (idx < 0) return 0;

  x11_fb_t* fb = &g_framebuffers[(size_t)idx];
  if (!fb->pixels || fb->width == 0 || fb->height == 0) return 0;

  if (outPixels) *outPixels = fb->pixels;
  if (outW) *outW = fb->width;
  if (outH) *outH = fb->height;
  return 1;
}

// Use the existing deferral logic (mapped/presentable gating) in C
void x11_xproto_enqueue_damage(uint32_t xid)
{
  enqueue_damage_window(xid);
}



void x11_xproto_bridge_destroy_window_legacy(uint32_t wid)
{
  if (wid == 0) return;

  // Mirror your existing handle_DestroyWindow logic:
  for (size_t i = 0; i < g_wins_n; i++) {
    if (g_wins[i].xid == wid) {

      // delete properties
      prop_delete_all_for_window(wid);

      // free framebuffer for this slot
      if (g_framebuffers[i].pixels) {
        free(g_framebuffers[i].pixels);
        g_framebuffers[i].pixels = NULL;
      }

      // swap-with-last, keep aligned
      size_t last = g_wins_n - 1;
      if (i != last) {
        g_wins[i] = g_wins[last];
        g_framebuffers[i] = g_framebuffers[last];
      }
      g_wins_n--;
      break;
    }
  }

  // NOTE: do NOT call enqueue_destroy_window() here anymore.
  // C++ owns that now via x11_requests_push_destroy() (or enqueue_destroy_window if you prefer).
}


// in x11_xproto.c
void x11_xproto_c_window_set_mapped(uint32_t xid, int mapped) {
  x11_win_t* w = win_find(xid);
  if (!w) return;
  w->mapped = mapped ? 1 : 0;
}

// in x11_xproto.c
void x11_xproto_c_window_set_presentable(uint32_t xid, int presentable) {
  x11_win_t* w = win_find(xid);
  if (!w) return;
  w->presentable = presentable ? 1 : 0;
}



// Minimal bridge: keep C-side w->event_mask in sync while we migrate handlers to C++.
void x11_xproto_c_set_window_event_mask(uint32_t xid, uint32_t event_mask)
{
  x11_win_t* w = win_find(xid);
  if (!w) return;
  w->event_mask = event_mask;
}

int x11_xproto_apply_map_window(uint32_t wid)
{
  if (wid == 0) return 0;
  x11_win_t* w = win_find(wid);
  if (!w) return 0;

  // Match old handle_MapWindow behavior:
  w->mapped = 1;
  enqueue_map_window(wid);

  // If anything drew while unmapped, flush exactly once now,
  // but only if presentable.
  if (w->dirty && w->presentable) {
    w->dirty = 0;
    enqueue_damage_window(wid);
  }

  return 1;
}


// x11_xproto.c
int x11_xproto_window_set_mapped(uint32_t wid, int mapped)
{
  x11_win_t* w = win_find(wid);
  if (!w) return 0;
  w->mapped = mapped ? 1 : 0;
  if (!mapped) w->dirty = 1; // matches your existing behavior
  return 1;
}


extern int x11_proto_bridge_send_reply_bytes(const void* buf, size_t n);

static int x11_send_all(int fd, const void* buf, size_t n) {
  // fd ignored: transport owns the active client fd
  return x11_proto_bridge_send_reply_bytes(buf, n);
}

static int x11_recv_all(int fd, void* buf, size_t n)
{
  uint8_t* p = (uint8_t*)buf;
  while (n) {
    ssize_t r = recv(fd, p, n, MSG_WAITALL);
    if (r == 0) return 0;
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
      return -1;
    }
    p += (size_t)r;
    n -= (size_t)r;
  }
  return 1;
}




#ifndef NDEBUG
// Debug: log header length_words and implied total bytes.
// Use this when you send a reply in multiple chunks (header + body).
static void dbg_check_reply_header32(const char* op, uint16_t seq, const uint8_t* rep32)
{
  uint32_t length_words = rd32(rep32 + 4);
  fprintf(stderr,
          "[SwiftX11] xproto: REPLY HDR op=%s seq=%u length_words=%u total=%zu\n",
          op, (unsigned)seq, (unsigned)length_words, 32u + (size_t)length_words * 4u);
}

// Debug: verify the *total* bytes sent for a reply matches header length_words.
// Use this only when you send the entire reply contiguously in one buffer.
static void dbg_check_reply_total(const char* op, uint16_t seq, size_t total_bytes, const uint8_t* rep32)
{
  uint32_t length_words = rd32(rep32 + 4);
  size_t expected = 32u + (size_t)length_words * 4u;
  if (total_bytes != expected) {
    fprintf(stderr,
            "[SwiftX11] xproto: REPLY LEN MISMATCH op=%s seq=%u bytes=%zu expected=%zu length_words=%u\n",
            op, (unsigned)seq, total_bytes, expected, (unsigned)length_words);
  }
}
#endif


// ************ this entire block should be deleted once we have verified C++ ********
// ----------------------------------------------------------------------------
// Server-thread -> xproto-thread notify queue (to avoid cross-thread socket writes)
// ----------------------------------------------------------------------------



static void x11_send_setup_failed_le(int fd, const char* reason)
{
  if (!reason) reason = "not implemented";

  uint8_t reason_len = (uint8_t)strnlen(reason, 255);
  uint16_t major = 11, minor = 0;

  uint16_t reason_padded = (uint16_t)((reason_len + 3u) & ~3u);
  uint16_t length_words  = (uint16_t)(reason_padded / 4u);

  uint8_t hdr[8] = {0};
  hdr[0] = 0;
  hdr[1] = reason_len;
  hdr[2] = (uint8_t)(major & 0xFF);
  hdr[3] = (uint8_t)((major >> 8) & 0xFF);
  hdr[4] = (uint8_t)(minor & 0xFF);
  hdr[5] = (uint8_t)((minor >> 8) & 0xFF);
  hdr[6] = (uint8_t)(length_words & 0xFF);
  hdr[7] = (uint8_t)((length_words >> 8) & 0xFF);

  (void)x11_send_all_fd(fd, hdr, sizeof(hdr));
  if (reason_len) (void)x11_send_all_fd(fd, reason, reason_len);

  if (reason_padded > reason_len) {
    static const uint8_t zeros[4] = {0,0,0,0};
    uint16_t pad = (uint16_t)(reason_padded - reason_len);
    while (pad) {
      uint16_t chunk = (pad > 4) ? 4 : pad;
      (void)x11_send_all_fd(fd, zeros, chunk);
      pad -= chunk;
    }
  }
}

// Minimal SetupSuccess reply sufficient to get real clients to start sending requests.
// Little-endian only for now; uses unaligned stores.
static void x11_send_setup_success_minimal_little_endian(int fd)
{
  // ---- Tunables / IDs
  const uint16_t proto_major = 11;
  const uint16_t proto_minor = 0;
  const uint32_t rid_base    = 0x10000000u;
  const uint32_t rid_mask    = 0x0FFFFFFFu;
  const uint32_t root_xid    = 0x00000001u;
  const uint32_t root_visid  = 0x00000021u;
  const uint32_t root_cmap   = 0x00000020u;

  const uint16_t screen_w_px = 800;
  const uint16_t screen_h_px = 600;
  const uint16_t screen_w_mm = 270;
  const uint16_t screen_h_mm = 203;

  const char* vendor = "SwiftX11";
  const uint16_t vendor_len = (uint16_t)strlen(vendor);
  const uint16_t vendor_pad = (uint16_t)((vendor_len + 3u) & ~3u);

  // ---- Sizes of variable blocks
  // Advertise formats clients expect (notably depth=1 for bitmaps).
  const uint8_t num_formats = 3;
  const uint8_t num_roots   = 1;

  const size_t fmt_bytes   = (size_t)num_formats * 8u;      // xPixmapFormat
  const size_t depth_bytes = 8u /*xDepth*/ + 24u /*xVisualType*/;
  const size_t root_bytes  = 40u /*xWindowRoot*/ + depth_bytes;

  const size_t setup_bytes =
      32u /*xConnSetup*/ +
      (size_t)vendor_pad +
      fmt_bytes +
      root_bytes;

  const uint16_t length_words = (uint16_t)(setup_bytes / 4u);

  // ---- Build reply in a single buffer
  const size_t total_bytes = 8u /*header*/ + setup_bytes;
  uint8_t* out = (uint8_t*)calloc(1, total_bytes);
  if (!out) return;

  size_t off = 0;

  // SetupSuccess header (8 bytes)
  // byte 0: status=1
  // byte 1: unused
  // bytes 2-3: protocol major
  // bytes 4-5: protocol minor
  // bytes 6-7: length (4-byte units) of data following this 8-byte header
  out[0] = 1;
  out[1] = 0;
  out[2] = (uint8_t)(proto_major & 0xFF);
  out[3] = (uint8_t)((proto_major >> 8) & 0xFF);
  out[4] = (uint8_t)(proto_minor & 0xFF);
  out[5] = (uint8_t)((proto_minor >> 8) & 0xFF);
  out[6] = (uint8_t)(length_words & 0xFF);
  out[7] = (uint8_t)((length_words >> 8) & 0xFF);
  off = 8;

  // xConnSetup (32 bytes)
  // release_number
  wr32_le(out + off + 0, 1);
  // resource_id_base / mask
  wr32_le(out + off + 4, rid_base);
  wr32_le(out + off + 8, rid_mask);
  // motion_buffer_size
  wr32_le(out + off + 12, 0);
  // nbytesVendor
  wr16_le(out + off + 16, vendor_len);
  // max_request_size (in 4-byte units)
  wr16_le(out + off + 18, 0xFFFF);
  
  // numRoots / numFormats
  out[off + 20] = num_roots;
  out[off + 21] = num_formats;
  // imageByteOrder / bitmapBitOrder
  out[off + 22] = 0; // LSBFirst
  out[off + 23] = 0; // LSBFirst
  // bitmapScanlineUnit / bitmapScanlinePad
  out[off + 24] = 32;
  out[off + 25] = 32;
  // minKeyCode / maxKeyCode
  out[off + 26] = 8;
  out[off + 27] = 255;
  // pad (4 bytes) already zero
  off += 32;

  // vendor string (padded)
  memcpy(out + off, vendor, vendor_len);
  off += vendor_pad;

  // xPixmapFormat list (8 bytes each)
  // #1: depth=1, bpp=1, scanline_pad=32 (bitmaps / 1bpp pixmaps)
  out[off + 0] = 1;
  out[off + 1] = 1;
  out[off + 2] = 32;
  off += 8;

  // #2: depth=24, bpp=32, scanline_pad=32 (our main window buffers)
  out[off + 0] = 24;
  out[off + 1] = 32;
  out[off + 2] = 32;
  off += 8;

  // #3: depth=32, bpp=32, scanline_pad=32 (some clients create 32-depth pixmaps)
  out[off + 0] = 32;
  out[off + 1] = 32;
  out[off + 2] = 32;
  off += 8;
 
  // IMPORTANT: whitePixel and blackPixel must be distinct.
  // Many clients derive default GC colors from these values.
  // xWindowRoot (40 bytes)
  wr32_le(out + off +  0, root_xid);     // root
  wr32_le(out + off +  4, root_cmap);    // defaultColormap
  wr32_le(out + off +  8, 1);            // whitePixel
  wr32_le(out + off + 12, 0);           // blackPixel
  wr32_le(out + off + 16, 0);           // currentInputMasks
  wr16_le(out + off + 20, screen_w_px); 
  wr16_le(out + off + 22, screen_h_px); 
  wr16_le(out + off + 24, screen_w_mm); 
  wr16_le(out + off + 26, screen_h_mm); 
  wr16_le(out + off + 28, 1);           // minInstalledMaps
  wr16_le(out + off + 30, 1);           // maxInstalledMaps
  wr32_le(out + off + 32, root_visid);  // rootVisualID
  out[off + 36] = 0;                         // backingStores
  out[off + 37] = 0;                         // saveUnders
  out[off + 38] = 24;                        // rootDepth
  out[off + 39] = 1;                         // nDepths
  off += 40;

  // xDepth (8 bytes): depth=24, nVisuals=1
  out[off + 0] = 24;
  out[off + 1] = 0;
  wr16_le(out + off + 2, 1);           // nVisuals
  // pad 4 bytes
  off += 8;

  // xVisualType (24 bytes): TrueColor visual
  wr32_le(out + off + 0, root_visid);        // visualid
  wr16_le(out + off + 6, 256);               // colormapEntries
  wr32_le(out + off + 8, 0x00FF0000u);       // redMask
  wr32_le(out + off + 12, 0x0000FF00u);      // greenMask
  wr32_le(out + off + 16, 0x000000FFu);      // blueMask
  out[off +  4] = 4;                         // class = TrueColor
  out[off +  5] = 8;                         // bitsPerRGB
  // pad 4 bytes
  off += 24;

  // Defensive: ensure we filled exactly what we computed.
  if (off != total_bytes) {
#ifndef NDEBUG
    fprintf(stderr, "[SwiftX11] xproto: SetupSuccess size mismatch off=%zu total=%zu\n", off, total_bytes);
#endif
    // still try sending what we built
  }

  (void)x11_send_all_fd(fd, out, total_bytes);
  free(out);
}

// ----------------------------------------------------------------------------
// Helpers: build 32-byte reply header
// ----------------------------------------------------------------------------
static void x11_reply32_le(uint8_t out[32], uint16_t seq, uint32_t extra_words)
{
  memset(out, 0, 32);
  out[0] = 1;
  out[2] = (uint8_t)(seq & 0xFF);
  out[3] = (uint8_t)((seq >> 8) & 0xFF);
  out[4] = (uint8_t)(extra_words & 0xFF);
  out[5] = (uint8_t)((extra_words >> 8) & 0xFF);
  out[6] = (uint8_t)((extra_words >> 16) & 0xFF);
  out[7] = (uint8_t)((extra_words >> 24) & 0xFF);
}

// ----------------------------------------------------------------------------
// Tiny Atom table (enough for InternAtom/GetAtomName)
// ----------------------------------------------------------------------------
typedef struct {
  uint32_t atom;
  char* name;
  size_t len;   // atom name length (bytes)
} atom_entry_t;


// ----------------------------------------------------------------------------
// Request handlers
// ----------------------------------------------------------------------------

static void handle_QueryExtension(int fd, uint16_t seq)
{
  // Reply: present=0, no opcodes/events/errors
  uint8_t rep[32];
  x11_reply32_le(rep, seq, 0);
  rep[1]  = 0; // present?
  rep[8]  = 0; // major_opcode
  rep[9]  = 0; // first_event
  rep[10] = 0; // first_error
  
#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] xproto: REPLY op=handle_QueryExtension seq=%u bytes=%zu length_words=%u\n",
          (unsigned)seq,
          (size_t)sizeof(rep),
          (unsigned)rd32(rep + 4));
  dbg_check_reply_total("QueryExtension", seq, 32, rep);
#endif
  
  (void)x11_send_all(fd, rep, sizeof(rep));
}

static void handle_ListExtensions(int fd, uint16_t seq)
{
  // Reply: nExtensions=0, length=0
  uint8_t rep[32];
  x11_reply32_le(rep, seq, 0);
  rep[1] = 0; // nExtensions
#ifndef NDEBUG
  dbg_check_reply_total("ListExtensions", seq, 32, rep);
#endif
  (void)x11_send_all(fd, rep, sizeof(rep));
}


// QueryColors (major = 91)
static void handle_QueryColors(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
  // Body after 4-byte header:
  //   CARD32 colormap
  //   LISTofCARD32 pixels
  // Reply returns a LISTofxrgb where xrgb is 8 bytes:
  //   CARD16 red, CARD16 green, CARD16 blue, CARD16 pad
  // (No pixel field in the reply; pixel list is already in the request.)
  if (remain < 4) return;

  // Number of pixels is implied by request length.
  uint16_t ncolors = (uint16_t)((remain - 4u) / 4u);
  if (ncolors > 1024) ncolors = 1024;

  // Each xrgb is 8 bytes = 2 words.
  const uint32_t extra_words = (uint32_t)ncolors * 2u;

  uint8_t rep[32];
  x11_reply32_le(rep, seq, extra_words);

  // Reply: bytes 8..9 = nColors (CARD16)
  rep[8] = (uint8_t)(ncolors & 0xFF);
  rep[9] = (uint8_t)((ncolors >> 8) & 0xFF);

#if !defined(NDEBUG) && SWIFTX11_TRACE
  fprintf(stderr, "[SwiftX11] xproto: QueryColors nColors=%u extra_words=%u remain=%zu\n",
          (unsigned)ncolors, (unsigned)extra_words, remain);
#endif

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] xproto: REPLY op=QueryColors (first x11_send_all) seq=%u bytes=%zu length_words=%u\n",
          (unsigned)seq,
          (size_t)sizeof(rep),
          (unsigned)rd32(rep + 4));
  dbg_check_reply_header32("QueryColors", seq, rep);
#endif
  (void)x11_send_all(fd, rep, sizeof(rep));

  // Minimal colormap behavior for bring-up:
  // Treat pixel==0 as black, and any nonzero pixel as white.
  for (uint16_t i = 0; i < ncolors; i++) {
    const uint32_t pix = rd32(payload + 4u + (size_t)i * 4u);

    // xrgb: CARD16 red, green, blue, pad
    uint8_t out[8];
    if (pix == 0) {
      // black
      out[0] = out[1] = 0;
      out[2] = out[3] = 0;
      out[4] = out[5] = 0;
    } else {
      // white
      out[0] = out[1] = 0xFF;
      out[2] = out[3] = 0xFF;
      out[4] = out[5] = 0xFF;
    }
    out[6] = 0;
    out[7] = 0;

    (void)x11_send_all(fd, out, sizeof(out));
  }
#ifndef NDEBUG
size_t total_sent = 32u + (size_t)ncolors * 8u;
dbg_check_reply_total("QueryColors(total)", seq, total_sent, rep);
fprintf(stderr, "[SwiftX11] xproto: REPLY TOTAL op=QueryColors seq=%u total_sent=%zu\n",
        (unsigned)seq, total_sent);
#endif
  
}


static void resize_window_and_fb(uint32_t wid, uint16_t new_w, uint16_t new_h)
{
  if (wid == 0) return;
  if (new_w == 0) new_w = 1;
  if (new_h == 0) new_h = 1;

  x11_win_t* w = win_find(wid);
  if (!w) return;

  const uint16_t old_w = w->w;
  const uint16_t old_h = w->h;

  if (new_w == old_w && new_h == old_h) return;

  w->w = new_w;
  w->h = new_h;

  const ssize_t idx = win_index(wid);
  if (idx < 0) return;

  x11_fb_t* fb = &g_framebuffers[(size_t)idx];
  const uint32_t new_fb_w = (uint32_t)new_w;
  const uint32_t new_fb_h = (uint32_t)new_h;
  const size_t new_npx = (size_t)new_fb_w * (size_t)new_fb_h;

  // Allocate-first then swap, so we don't lose old buffer on malloc failure
  uint32_t* new_pixels = (uint32_t*)malloc(new_npx * sizeof(uint32_t));
  if (!new_pixels) return;

  // Initialize new buffer to white.
  for (size_t i = 0; i < new_npx; i++) {
    new_pixels[i] = 0xFFFFFFFFu;
  }

  // Preserve old contents in the overlapping region.
  const uint32_t old_fb_w = fb->width;
  const uint32_t old_fb_h = fb->height;
  const uint32_t copy_w = (old_fb_w < new_fb_w) ? old_fb_w : new_fb_w;
  const uint32_t copy_h = (old_fb_h < new_fb_h) ? old_fb_h : new_fb_h;

  if (fb->pixels && copy_w && copy_h) {
    for (uint32_t yy = 0; yy < copy_h; yy++) {
      const uint32_t* src_row = fb->pixels + (size_t)yy * (size_t)old_fb_w;
      uint32_t* dst_row = new_pixels + (size_t)yy * (size_t)new_fb_w;
      memcpy(dst_row, src_row, (size_t)copy_w * sizeof(uint32_t));
    }
  }

  free(fb->pixels);
  fb->pixels = new_pixels;
  fb->width  = new_fb_w;
  fb->height = new_fb_h;
}


void x11_xproto_apply_rootless_resize_on_server_thread(uint32_t wid,
                                                       int32_t w_px,
                                                       int32_t h_px)
{
#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] entered xproto_apply_rootless_resize: wid=%d\n",
        wid);
#endif
  
  if (wid == 0) return;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  x11_win_t* w = win_find(wid);
  if (!w) return;

#ifndef NDEBUG
fprintf(stderr, "[SwiftX11] xproto_apply_rootless_resize: xid=0x%08X %dx%d\n",
        (unsigned)wid, (int)w_px, (int)h_px);
#endif
  
  const uint16_t old_w = w->w;
  const uint16_t old_h = w->h;

#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] xproto_apply_rootless_resize (old): xid=0x%08X %dx%d\n",
        (unsigned)wid, (int)old_w, (int)old_h);
#endif

  
  // Update server-truth geometry (Cocoa authoritative)
  uint16_t new_w = (uint16_t)w_px;
  uint16_t new_h = (uint16_t)h_px;
  if (new_w == 0) new_w = 1;
  if (new_h == 0) new_h = 1;

#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] xproto_apply_rootless_resize (new): xid=0x%08X %dx%d\n",
        (unsigned)wid, (int)new_w, (int)new_h);
#endif

  // If nothing changed, you can early-out (or still damage if you want)
  if (new_w == old_w && new_h == old_h) {
    // Optional: enqueue_damage_window(wid);
    return;
  }

  // compute the scale factors
  float sx = (old_w > 0) ? ((float)new_w / (float)old_w) : 1.0f;
  float sy = (old_h > 0) ? ((float)new_h / (float)old_h) : 1.0f;
  
  // Resize the top-level window + framebuffer.
  resize_window_and_fb(wid, new_w, new_h);
  // Keep C++ authoritative WindowTable in sync
  x11_proto_bridge_window_set_geometry(wid, w->x, w->y, new_w, new_h);
  
  // Notify the owning client that geometry changed, so it can recompute & redraw.
  // IMPORTANT: do NOT write to the client socket from this thread.
  // Queue the notifications and let the xproto thread flush them.
  {
    x11_win_t* ww = win_find(wid);
    if (ww && (new_w != old_w || new_h != old_h)) {
      const int want_cfg = (ww->event_mask & (1u << 17)) ? 1 : 0; // StructureNotifyMask
      const int want_exp = (ww->event_mask & (1u << 15)) ? 1 : 0; // ExposureMask
      if (want_cfg || want_exp) {
#ifndef NDEBUG
fprintf(stderr,
        "[SwiftX11] rootless_resize: QUEUE host notify wid=0x%08X cfg=%d exp=%d mask=0x%08X\n",
        (unsigned)wid, want_cfg, want_exp,
        ww ? (unsigned)ww->event_mask : 0u);
#endif
        x11_proto_bridge_queue_notify(wid, want_cfg, want_exp);
      }
    }
  }
  
  // Rootless-resize child handling:
  // Prefer correctness by propagating geometry changes to children rather than
  // scaling pixels.
  //
  // Rules (direct children of `wid` only):
  //  1) If a child exactly covered the old parent at (0,0), keep it covering.
  //  2) Else if a child is anchored at (0,0), scale its size.
  //  3) Else scale both position and size.
  //
  // NOTE: This is a heuristic; real X11 toolkits often manage layout via
  // ConfigureWindow from the client, but rootless hosts commonly need this.

  for (size_t i = 0; i < g_wins_n; i++) {
    x11_win_t* c = &g_wins[i];
    if (!c->xid) continue;
    if (c->parent != wid) continue;

    // Snapshot old child geometry.
    const int16_t  cx0 = c->x;
    const int16_t  cy0 = c->y;
    const uint16_t cw0 = c->w;
    const uint16_t ch0 = c->h;

    // Compute new child geometry.
    int16_t  cx1 = cx0;
    int16_t  cy1 = cy0;
    uint16_t cw1 = cw0;
    uint16_t ch1 = ch0;

    const int covered_old_parent = (cx0 == 0 && cy0 == 0 && cw0 == old_w && ch0 == old_h);
    const int anchored_origin    = (cx0 == 0 && cy0 == 0);

    if (covered_old_parent) {
      // Case 1: canonical covering child.
      cx1 = 0;
      cy1 = 0;
      cw1 = new_w;
      ch1 = new_h;
    } else if (anchored_origin) {
      // Case 2: anchored at origin; scale size.
      // Use the host scale factors computed above.
      int nw = (int)lroundf((float)cw0 * sx);
      int nh = (int)lroundf((float)ch0 * sy);
      if (nw < 1) nw = 1;
      if (nh < 1) nh = 1;
      cw1 = (uint16_t)nw;
      ch1 = (uint16_t)nh;
    } else {
      // Case 3: scale both position and size.
      int nx = (int)lroundf((float)cx0 * sx);
      int ny = (int)lroundf((float)cy0 * sy);
      int nw = (int)lroundf((float)cw0 * sx);
      int nh = (int)lroundf((float)ch0 * sy);
      if (nw < 1) nw = 1;
      if (nh < 1) nh = 1;
      cx1 = (int16_t)nx;
      cy1 = (int16_t)ny;
      cw1 = (uint16_t)nw;
      ch1 = (uint16_t)nh;
    }

    // No-op if nothing changed.
    if (cx1 == cx0 && cy1 == cy0 && cw1 == cw0 && ch1 == ch0) {
      continue;
    }

#ifndef NDEBUG
    fprintf(stderr,
            "[SwiftX11] rootless_resize: child xid=0x%08X (%d,%d %ux%u) -> (%d,%d %ux%u) host old %ux%u new %ux%u\n",
            (unsigned)c->xid,
            (int)cx0, (int)cy0, (unsigned)cw0, (unsigned)ch0,
            (int)cx1, (int)cy1, (unsigned)cw1, (unsigned)ch1,
            (unsigned)old_w, (unsigned)old_h,
            (unsigned)new_w, (unsigned)new_h);
#endif

    // Apply geometry change in xproto truth.
    c->x = cx1;
    c->y = cy1;

    // Resize backing FB (and update w/h) using the shared helper.
    resize_window_and_fb(c->xid, cw1, ch1);
    x11_proto_bridge_window_set_geometry(c->xid, c->x, c->y, cw1, ch1);
    fprintf(stderr, "[SwiftX11] rootless_resize: resized child xid=0x%08X fb now %ux%u\n",
            (unsigned)c->xid,
            (unsigned)g_framebuffers[win_index(c->xid)].width,
            (unsigned)g_framebuffers[win_index(c->xid)].height);
    
    // Notify client on xproto thread.
    {
      x11_win_t* cc = win_find(c->xid);
      if (cc) {
        const int want_cfg = (cc->event_mask & (1u << 17)) ? 1 : 0; // StructureNotifyMask
        const int want_exp = (cc->event_mask & (1u << 15)) ? 1 : 0; // ExposureMask
#ifndef NDEBUG
fprintf(stderr,
        "[SwiftX11] rootless_resize: QUEUE child notify wid=0x%08X cfg=%d exp=%d mapped=%d mask=0x%08X parent=0x%08X\n",
        (unsigned)cc->xid, want_cfg, want_exp,
        (int)cc->mapped,
        (unsigned)cc->event_mask,
        (unsigned)cc->parent);
#endif
        if (want_cfg || want_exp) {
          x11_proto_bridge_queue_notify(cc->xid, want_cfg, want_exp);
        }
      }
    }

    // Ensure the UI presents updated contents.
    enqueue_damage_window(c->xid);

    // Also enqueue a ConfigureWindow event to the shim so Cocoa/Swift can track size.
    // (No socket write; this just feeds the Swift side.)
    enqueue_configure_window(c->xid, c->x, c->y, c->w, c->h);
  }
  
  // Mark the host as damaged too.
  enqueue_damage_window(wid);
  
  
#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] ROOTLESS_RESIZE apply xid=0x%08X %u×%u (old %u×%u)\n",
          (unsigned)wid, (unsigned)new_w, (unsigned)new_h,
          (unsigned)old_w, (unsigned)old_h);
#endif
}

void x11_xproto_apply_configure_from_cpp(uint32_t wid,
                                         int16_t x, int16_t y,
                                         uint16_t w, uint16_t h,
                                         int resize_fb)
{
  if (wid == 0) return;
  if (w == 0) w = 1;
  if (h == 0) h = 1;

  x11_win_t* ww = win_find(wid);
  if (!ww) return;

  const uint16_t old_w = ww->w;
  const uint16_t old_h = ww->h;

  ww->x = x;
  ww->y = y;
  ww->w = w;
  ww->h = h;

  if (resize_fb && (w != old_w || h != old_h)) {
    // reuse your existing helper (it already allocs white + preserves overlap)
    resize_window_and_fb(wid, w, h);
    // resizing implies damage (deferred if not ready)
    enqueue_damage_window(wid);
  }
}



// ----------------------------------------------------------------------------
// Swift-side server to X11
// ----------------------------------------------------------------------------
void x11_xproto_set_window_presentable(uint32_t xid)
{
  if (xid == 0) return;
  
  x11_win_t* w = win_find(xid);
  if (!w) return;
  
  x11_proto_bridge_window_set_presentable(xid, 1);
  
  if (x11_proto_bridge_window_consume_dirty_if_ready(xid)) {
    enqueue_damage_window(xid);
  }
}



// ----------------------------------------------------------------------------
// Request pump + dispatcher
// ----------------------------------------------------------------------------
void drain_requests(int cfd)
{
  // Small recv timeout so we can notice stop without blocking forever.
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100 * 1000;
  (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv));

  uint16_t seq = 0;
  g_current_client_fd = cfd;
  // Record the xproto client thread for cross-thread safety checks.
  g_xproto_thread = pthread_self();
  g_xproto_thread_valid = 1;
  dbg_set_xproto_thread(pthread_self());
  
  bool should_cleanup = true;

  for (;;) {
    // Flush any pending synthetic events requested by other threads.
    x11_proto_bridge_flush_notify_queue();
    //    flush_notify_queue(cfd);
    if (atomic_load_explicit(&g_stop, memory_order_relaxed)) {
      should_cleanup = false;
      break; // stop requested -> break out to disconnect cleanup
    }

    uint8_t hdr[4];
    int hr = x11_recv_all(cfd, hdr, sizeof(hdr));
    if (hr == 0) break;        // client closed connection
    if (hr == -2) continue;    // timeout, retry
    if (hr < 0) break;         // real socket error


    const uint8_t major = hdr[0];
    const uint8_t minor = hdr[1];
    const uint16_t len_words = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));
    if (len_words == 0) break;

    const size_t total = (size_t)len_words * 4u;
    if (total < 4u) break;
    const size_t remain = total - 4u;

    seq++;
    x11_proto_bridge_note_last_seq(seq);
//    atomic_store_explicit(&g_last_seq, seq, memory_order_relaxed);
    
    uint8_t stack_buf[4096];
    uint8_t* payload = stack_buf;
    uint8_t* heap_buf = NULL;

    // Try to allocate if body is larger than stack buffer
    if (remain > sizeof(stack_buf)) {
      heap_buf = malloc(remain);
      if (!heap_buf) {
        // allocation failed -> break entire loop
        break;
      }
      payload = heap_buf;
    }


    // read request body
    int rr = x11_recv_exact(cfd, payload, remain);
    if (rr == 1) {
      // ok
    } else if (rr == -2) {
      // timeout while reading body — treat as disconnect for now (safe)
      break;
    } else {
      // rr==0 EOF or rr<0 fatal => break out (disconnect)
      break;
    }
    
    
    
//#ifndef NDEBUG
if (major == 62 || major == 63) {
  fprintf(stderr, "[SwiftX11] DISPATCH major=%u (Copy%s)\n",
          (unsigned)major, (major == 62) ? "Area" : "Plane");
}
if (major >= 128) {
  fprintf(stderr, "[SwiftX11] DISPATCH extension opcode=%u minor=%u len_words=%u\n",
          (unsigned)major, (unsigned)minor, (unsigned)len_words);
}
//zThe#endif    
    // Dispatch
    int handled = x11_proto_bridge_dispatch(major, minor, seq, payload, remain);
    if (!handled) {
      
      switch (major) {
                    
        case 91: // QueryColors
          handle_QueryColors(cfd, seq, payload, remain);
          break;
          
        case 98: // QueryExtension
          handle_QueryExtension(cfd, seq);
          break;
          
        case 99: // ListExtensions
          handle_ListExtensions(cfd, seq);
          break;
          
        default:
#ifndef NDEBUG
          fprintf(stderr,
                  "[SwiftX11] xproto: UNHANDLED major=%u minor=%u len_words=%u remain=%zu\n******************************************************************************************\n",
                  (unsigned)major, (unsigned)minor, (unsigned)len_words, remain);
#endif
          break;
      }
    }
    
    // Flush again after handling a request so synthetic events don't backlog behind traffic.
    x11_proto_bridge_flush_notify_queue();
    //flush_notify_queue(cfd);
    
    // Always free heap_buf if used
    if (heap_buf) {
      fprintf(stderr, "[SwiftX11] freeing heap_buf for major=%u\n", (unsigned)major);
      free(heap_buf);
      heap_buf = NULL;
    }
  }
  
  g_xproto_thread_valid = 0;
  g_current_client_fd = -1;
  
  if (should_cleanup) {
    // Client disconnected: destroy all windows owned by this client
    for (size_t i = 0; i < g_wins_n; ) {
      if (g_wins[i].owner_fd == cfd) {
        uint32_t wid = g_wins[i].xid;
        x11_proto_bridge_window_erase(wid);

        prop_delete_all_for_window(wid);

        // free framebuffer
        if (g_framebuffers[i].pixels) {
          free(g_framebuffers[i].pixels);
          g_framebuffers[i].pixels = NULL;
        }

        // swap-with-last (keep aligned)
        size_t last = g_wins_n - 1;
        if (i != last) {
          g_wins[i] = g_wins[last];
          g_framebuffers[i] = g_framebuffers[last];
        }
        g_wins_n--;

        enqueue_destroy_window(wid);
        continue; // re-check swapped entry
      }
      i++;
    }
  }
  
  x11_proto_bridge_flush_notify_queue();
  //flush_notify_queue(cfd);
  
//#if !defined(NDEBUG) && SWIFTX11_TRACE
  fprintf(stderr, "[SwiftX11] xproto: 787878787878787878787878778787878787878 drain_requests exiting (client closed or error)\n");
//#endif
}

// ----------------------------------------------------------------------------
// Listener thread
// ----------------------------------------------------------------------------
static void* listener_main(void* _)
{
  (void)_;

  for (;;) {
    if (atomic_load_explicit(&g_stop, memory_order_relaxed)) break;

    int lfd = g_lfd;
    if (lfd < 0) break;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(lfd, &rfds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100 * 1000;

    int sel = select(lfd + 1, &rfds, NULL, NULL, &tv);
    if (sel <= 0) continue;

    struct sockaddr_in addr;
    socklen_t alen = (socklen_t)sizeof(addr);
    int cfd = accept(lfd, (struct sockaddr*)&addr, &alen);
    if (cfd < 0) continue;

#if defined(SO_NOSIGPIPE)
    int one = 1;
    (void)setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, (socklen_t)sizeof(one));
#endif

    // Read setup request (12 bytes)
    uint8_t req[12];
    ssize_t got = recv(cfd, req, sizeof(req), MSG_WAITALL);
    if (got != (ssize_t)sizeof(req)) {
      close(cfd);
      continue;
    }

    const char byte_order = (char)req[0];
    if (byte_order != 'l') {
      x11_send_setup_failed_le(cfd, "SwiftX11: only little-endian supported");
      close(cfd);
      continue;
    }

    // Skip auth
    uint16_t auth_proto_len = (uint16_t)(req[6] | ((uint16_t)req[7] << 8));
    uint16_t auth_data_len  = (uint16_t)(req[8] | ((uint16_t)req[9] << 8));
    size_t skip = 0;
    skip += ((size_t)auth_proto_len + 3u) & ~3u;
    skip += ((size_t)auth_data_len  + 3u) & ~3u;

    while (skip) {
      uint8_t buf[256];
      size_t want = (skip > sizeof(buf)) ? sizeof(buf) : skip;
      ssize_t r = recv(cfd, buf, want, MSG_WAITALL);
      if (r <= 0) break;
      skip -= (size_t)r;
    }
    if (skip != 0) { close(cfd); continue; }

#if !defined(NDEBUG) && SWIFTX11_TRACE
    fprintf(stderr, "[SwiftX11] xproto: client connected (byte_order=l), replying SetupSuccess(minimal)\n");
#endif

    x11_send_setup_success_minimal_little_endian(cfd);

    // ************ temporary bridge for C -> C++ transision **************
    x11_proto_bridge_begin_session(cfd);
    
    // Now drain requests
    drain_requests(cfd);

    x11_proto_bridge_end_session();
    close(cfd);
  }

  return NULL;
}

// ----------------------------------------------------------------------------
// Public start/stop
// ----------------------------------------------------------------------------
void x11_xproto_listener_start(int display)
{
  if (atomic_exchange_explicit(&g_running, 1, memory_order_acq_rel)) return;

  atomic_store_explicit(&g_stop, 0, memory_order_release);

  const int port = 6000 + display;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    atomic_store(&g_running, 0);
    return;
  }

  int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof(one));

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(fd, (struct sockaddr*)&sa, (socklen_t)sizeof(sa)) != 0) {
    close(fd);
    atomic_store(&g_running, 0);
    return;
  }

  if (listen(fd, 16) != 0) {
    close(fd);
    atomic_store(&g_running, 0);
    return;
  }

  g_lfd = fd;

#if !defined(NDEBUG) && SWIFTX11_TRACE
  fprintf(stderr, "[SwiftX11] xproto: listening on 127.0.0.1:%d (display :%d)\n", port, display);
#endif

  if (pthread_create(&g_thread, NULL, listener_main, NULL) != 0) {
    close(fd);
    g_lfd = -1;
    atomic_store(&g_running, 0);
    return;
  }
}

void x11_xproto_listener_stop(void)
{
  if (!atomic_load(&g_running)) return;

  atomic_store_explicit(&g_stop, 1, memory_order_release);

  if (g_lfd >= 0) {
    close(g_lfd); // breaks select/accept
    g_lfd = -1;
  }

  pthread_join(g_thread, NULL);
  atomic_store(&g_running, 0);

#if !defined(NDEBUG) && SWIFTX11_TRACE
  fprintf(stderr, "[SwiftX11] xproto: listener stopped\n");
#endif

}


// Copies current framebuffer bytes for xid into out_bytes.
// Returns 1 on success, 0 on failure.
// If out_bytes is NULL or out_cap too small, returns 0 but fills out_w/out_h/out_bpr.
int x11_backend_copy_window_bgra(uint32_t xid,
                                 uint8_t* out_bytes,
                                 int32_t out_cap,
                                 int32_t* out_w,
                                 int32_t* out_h,
                                 int32_t* out_bpr)
{
  const ssize_t idx = win_index(xid);
  x11_fb_t* fb = (idx >= 0) ? &g_framebuffers[(size_t)idx] : NULL;

  if (!fb || !fb->pixels || fb->width == 0 || fb->height == 0) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_bpr) *out_bpr = 0;
    return 0;
  }

  int32_t w = (int32_t)fb->width;
  int32_t h = (int32_t)fb->height;
  int32_t bpr = w * 4;
  int64_t needed64 = (int64_t)bpr * (int64_t)h;
  if (needed64 <= 0 || needed64 > INT32_MAX) return 0;
  int32_t needed = (int32_t)needed64;

  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  if (out_bpr) *out_bpr = bpr;

  if (!out_bytes || out_cap < needed) return 0;

#ifndef NDEBUG
  {
    // Sample a few pixels so we can confirm the server-drawn FB is what the UI copies.
    const int32_t sx_c = w / 2;
    const int32_t sy_c = h / 2;
    const int32_t sx_l = w / 4;
    const int32_t sx_r = (w * 3) / 4;
    const int32_t sy_m = h / 2;

    uint32_t sp_c = 0, sp_l = 0, sp_r = 0;
    if (sx_c >= 0 && sy_c >= 0 && sx_c < w && sy_c < h) {
      sp_c = fb->pixels[(size_t)sy_c * (size_t)fb->width + (size_t)sx_c];
    }
    if (sx_l >= 0 && sy_m >= 0 && sx_l < w && sy_m < h) {
      sp_l = fb->pixels[(size_t)sy_m * (size_t)fb->width + (size_t)sx_l];
    }
    if (sx_r >= 0 && sy_m >= 0 && sx_r < w && sy_m < h) {
      sp_r = fb->pixels[(size_t)sy_m * (size_t)fb->width + (size_t)sx_r];
    }

    // Count non-white pixels (very cheap sanity check for "did we draw anything?")
    size_t nonwhite = 0;
    const size_t npx = (size_t)fb->width * (size_t)fb->height;
    for (size_t i = 0; i < npx; i++) {
      if (fb->pixels[i] != 0xFFFFFFFFu) nonwhite++;
    }

    fprintf(stderr,
            "[SwiftX11] copy_window_bgra: xid=0x%08X fb=%p %dx%d nonwhite=%zu sample L/C/R=0x%08X 0x%08X 0x%08X\n",
            (unsigned)xid, (void*)fb->pixels, (int)w, (int)h,
            nonwhite, (unsigned)sp_l, (unsigned)sp_c, (unsigned)sp_r);
  }
#endif

  memcpy(out_bytes, (const void*)fb->pixels, (size_t)needed);
  return 1;
}



