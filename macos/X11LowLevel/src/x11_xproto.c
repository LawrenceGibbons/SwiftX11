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

// Forward decls (used by enqueue helpers near top of file)
static const char* atoms_name(uint32_t atom, size_t* out_len);

// -----------------------------------------------------------------------------
// enqueue requests to be consumed by x11_shim (via x11_requests_* queue)
// -----------------------------------------------------------------------------
//static void enqueue_create_window(uint32_t xid, uint32_t parent, int16_t x, int16_t y,
//                                  uint16_t w, uint16_t h, uint32_t event_mask)
//{
//  (void)parent; (void)x; (void)y; (void)event_mask;
//  char title[64];
//  snprintf(title, sizeof(title), "xid=0x%08X", (unsigned)xid);
//  (void)x11_requests_push_create(xid, parent, title, (int32_t)w, (int32_t)h);
//}

static void enqueue_destroy_window(uint32_t xid)
{
  (void)x11_requests_push_destroy(xid);
}

static void enqueue_map_window(uint32_t xid)
{
  (void)x11_requests_push_map(xid);
}

static void enqueue_unmap_window(uint32_t xid)
{
  (void)x11_requests_push_unmap(xid);
}

static void enqueue_configure_window(uint32_t xid, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
  (void)x; (void)y;
  (void)x11_requests_push_configure(xid, (int32_t)w, (int32_t)h);
}

// Best-effort: translate WM_NAME / _NET_WM_NAME ChangeProperty into set_title.
static void enqueue_maybe_set_title(uint32_t xid, uint32_t property_atom,
                                   uint32_t type_atom, uint8_t format,
                                   const uint8_t* bytes, uint32_t nbytes)
{
  (void)type_atom;
  bool is_name_prop = (property_atom == 39); // WM_NAME predefined

  if (!is_name_prop) {
    size_t alen = 0;
    const char* aname = atoms_name(property_atom, &alen);
    if (aname && alen == strlen("_NET_WM_NAME") && memcmp(aname, "_NET_WM_NAME", alen) == 0) {
      is_name_prop = true;
    }
  }

  if (!is_name_prop) return;
  if (format != 8) return;

  const uint32_t cap = 4096;
  uint32_t n = (nbytes > cap) ? cap : nbytes;
  char* s = (char*)malloc((size_t)n + 1u);
  if (!s) return;
  if (n) memcpy(s, bytes, n);
  s[n] = 0;

  (void)x11_requests_push_set_title(xid, s);
  free(s);
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


static x11_pixmap_t g_pixmaps[X11_MAX_PIXMAPS];
static _Atomic int g_stop = 0;
static _Atomic int g_running = 0;
static int g_lfd = -1;
static pthread_t g_thread;
static int g_current_client_fd = -1;

static const uint32_t X11_ROOT_XID = 0x00000001u;
static const uint32_t X11_ROOT_VIS = 0x00000021u;

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

static uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t rd32(const uint8_t* p){ return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)); }

static ssize_t pix_index(uint32_t xid) {
  for (size_t i = 0; i < X11_MAX_PIXMAPS; i++) {
    if (g_pixmaps[i].xid == xid) return (ssize_t)i;
  }
  return -1;
}

static ssize_t pix_alloc(uint32_t xid) {
  ssize_t idx = pix_index(xid);
  if (idx >= 0) return idx;
  for (size_t i = 0; i < X11_MAX_PIXMAPS; i++) {
    if (g_pixmaps[i].xid == 0) {
      memset(&g_pixmaps[i], 0, sizeof(g_pixmaps[i]));
      g_pixmaps[i].xid = xid;
      return (ssize_t)i;
    }
  }
  return -1;
}

static void pix_free(uint32_t xid) {
  ssize_t idx = pix_index(xid);
  if (idx < 0) return;
  free(g_pixmaps[(size_t)idx].pixels);
  free(g_pixmaps[(size_t)idx].bits);
  memset(&g_pixmaps[(size_t)idx], 0, sizeof(g_pixmaps[(size_t)idx]));
}


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

static x11_prop_t* prop_find(uint32_t wid, uint32_t atom)
{
  for (size_t i = 0; i < g_props_n; i++)
    if (g_props[i].wid == wid && g_props[i].atom == atom) return &g_props[i];
  return NULL;
}

static void prop_delete(uint32_t wid, uint32_t atom)
{
  for (size_t i = 0; i < g_props_n; i++) {
    if (g_props[i].wid == wid && g_props[i].atom == atom) {
      free(g_props[i].data);
      g_props[i] = g_props[g_props_n - 1];
      g_props_n--;
      return;
    }
  }
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

static void prop_set_bytes(uint32_t wid, uint32_t atom, uint32_t type,
                           uint8_t format, const uint8_t* bytes, uint32_t nbytes)
{
  const uint32_t kMax = 1u << 20; // 1 MiB cap for bring-up
  if (nbytes > kMax) nbytes = kMax;

  x11_prop_t* p = prop_find(wid, atom);
  if (!p) {
    if (g_props_n >= (sizeof(g_props)/sizeof(g_props[0]))) return;
    p = &g_props[g_props_n++];
    memset(p, 0, sizeof(*p));
    p->wid = wid;
    p->atom = atom;
  }

  uint8_t* buf = NULL;
  if (nbytes) {
    buf = (uint8_t*)malloc(nbytes);
    if (!buf) return;
    memcpy(buf, bytes, nbytes);
  }

  free(p->data);
  p->data = buf;
  p->nbytes = nbytes;
  p->type = type;
  p->format = format;
}



static void prop_prepend_append(uint32_t wid, uint32_t atom, uint32_t type,
                                uint8_t format, const uint8_t* bytes, uint32_t nbytes,
                                int append)
{
  x11_prop_t* p = prop_find(wid, atom);
  if (!p || p->format != format || p->type != type) {
    prop_set_bytes(wid, atom, type, format, bytes, nbytes);
    return;
  }

  const uint32_t old_n = p->nbytes;
  const uint32_t kMax = 1u << 20;

  uint32_t new_n = old_n + nbytes;
  if (new_n > kMax) new_n = kMax;

  uint8_t* buf = (uint8_t*)malloc(new_n);
  if (!buf) return;

  if (append) {
    uint32_t a = old_n; if (a > new_n) a = new_n;
    if (a) memcpy(buf, p->data, a);
    uint32_t b = (new_n > a) ? (new_n - a) : 0;
    if (b) memcpy(buf + a, bytes, b);
  } else {
    uint32_t a = nbytes; if (a > new_n) a = new_n;
    if (a) memcpy(buf, bytes, a);
    uint32_t b = (new_n > a) ? (new_n - a) : 0;
    if (b) memcpy(buf + a, p->data, b);
  }

  free(p->data);
  p->data = buf;
  p->nbytes = new_n;
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
static _Atomic uint16_t g_last_seq = 0;

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

//int x11_xproto_query_tree(uint32_t wid,
//                          uint32_t* out_parent,
//                          uint32_t* out_children,
//                          uint32_t  max_children,
//                          uint32_t* out_nchildren)
//{
//  if (out_parent) *out_parent = 0;
//  if (out_nchildren) *out_nchildren = 0;
//
//  // Root window: parent=None
//  if (wid == X11_ROOT_XID) {
//    if (out_parent) *out_parent = 0;
//  } else {
//    x11_win_t* w = win_find(wid);
//    if (w) {
//      if (out_parent) *out_parent = w->parent;
//    } else {
//      // Unknown window
//      return 0;
//    }
//  }
//
//  if (!out_children || max_children == 0 || !out_nchildren) {
//    return 1;
//  }
//
//  uint32_t n = 0;
//  for (size_t i = 0; i < g_wins_n; i++) {
//    if (g_wins[i].parent == wid) {
//      if (n < max_children) {
//        out_children[n] = g_wins[i].xid;
//      }
//      n++;
//      if (n >= max_children) break;
//    }
//  }
//
//  *out_nchildren = n;
//  return 1;
//}


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


//static void send_Expose(int fd, uint16_t seq, uint32_t wid, uint16_t x, uint16_t y,
//                        uint16_t w, uint16_t h, uint16_t count){
//  // Expose event: type=12
//  uint8_t ev[32];
//  memset(ev, 0, sizeof(ev));
//  ev[0] = 12; // Expose
//  // ev[1] unused
//  wr16_le(ev + 2, seq);
//  wr32_le(ev + 4, wid);
//  wr16_le(ev + 8, x);
//  wr16_le(ev + 10, y);
//  wr16_le(ev + 12, w);
//  wr16_le(ev + 14, h);
//  wr16_le(ev + 16, count);
//  (void)x11_send_all(fd, ev, sizeof(ev));
//}


//static void send_ConfigureNotify(int fd, uint16_t seq, uint32_t wid,
//                                 int16_t x, int16_t y,
//                                 uint16_t w, uint16_t h,
//                                 uint16_t border_width)
//{
//  // ConfigureNotify is event type 22, 32 bytes
//  uint8_t ev[32];
//  memset(ev, 0, sizeof(ev));
//  ev[0] = 22; // ConfigureNotify
//  ev[1] = 0;  // unused (event is not synthetic)
//  // sequence is set by server 
//  wr16_le(ev + 2, seq);
//
//  // event and window are both 'wid' for a normal ConfigureNotify delivered to that window
//  wr32_le(ev + 4, wid);  // event
//  wr32_le(ev + 8, wid);  // window
//
//  wr32_le(ev + 12, 0);   // aboveSibling = None (0)
//
//  wr16_le(ev + 16, (uint16_t)x);
//  wr16_le(ev + 18, (uint16_t)y);
//  wr16_le(ev + 20, w);
//  wr16_le(ev + 22, h);
//  wr16_le(ev + 24, border_width);
//  ev[26] = 0;            // overrideRedirect = false
//  // rest pad
//
//  (void)x11_send_all(fd, ev, sizeof(ev));
//}


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

//typedef struct {
//  uint32_t wid;
//  uint8_t  want_configure;
//  uint8_t  want_expose;
//  uint8_t  _pad[2];
//} x11_pending_notify_t;
//
//static pthread_mutex_t g_pending_mu = PTHREAD_MUTEX_INITIALIZER;
//static x11_pending_notify_t g_pending[1024];
//static size_t g_pending_n = 0;
//
//static void queue_notify(uint32_t wid, int want_configure, int want_expose)
//{
//  if (wid == 0) return;
//
//  pthread_mutex_lock(&g_pending_mu);
//
//  // Coalesce by wid.
//  for (size_t i = 0; i < g_pending_n; i++) {
//    if (g_pending[i].wid == wid) {
//      if (want_configure) g_pending[i].want_configure = 1;
//      if (want_expose)    g_pending[i].want_expose = 1;
//      pthread_mutex_unlock(&g_pending_mu);
//      return;
//    }
//  }
//
//  if (g_pending_n < (sizeof(g_pending) / sizeof(g_pending[0]))) {
//    g_pending[g_pending_n].wid = wid;
//    g_pending[g_pending_n].want_configure = want_configure ? 1 : 0;
//    g_pending[g_pending_n].want_expose    = want_expose ? 1 : 0;
//    g_pending_n++;
//  }
//
//  pthread_mutex_unlock(&g_pending_mu);
//}

//// Flush queued notifications; MUST be called only from the xproto thread.
//static void flush_notify_queue(int fd)
//{
//  if (!g_xproto_thread_valid) return;
//  if (!pthread_equal(pthread_self(), g_xproto_thread)) return;
//
//  x11_pending_notify_t local[1024];
//  size_t n = 0;
//
//  pthread_mutex_lock(&g_pending_mu);
//  n = g_pending_n;
//  if (n > (sizeof(local) / sizeof(local[0]))) n = (sizeof(local) / sizeof(local[0]));
//  for (size_t i = 0; i < n; i++) local[i] = g_pending[i];
//  // compact: drop everything we copied
//  if (n == g_pending_n) {
//    g_pending_n = 0;
//  } else {
//    // shift remaining down
//    const size_t remain = g_pending_n - n;
//    for (size_t i = 0; i < remain; i++) g_pending[i] = g_pending[n + i];
//    g_pending_n = remain;
//  }
//  pthread_mutex_unlock(&g_pending_mu);
//
//  // THe last used equence number
//  uint16_t seq0 = atomic_load_explicit(&g_last_seq, memory_order_relaxed);
//  if (seq0 == 0) seq0 = 1;  // defensive: avoid “0 seq” before first request
//  
//  for (size_t i = 0; i < n; i++) {
//    const uint32_t wid = local[i].wid;
//    x11_win_t* w = win_find(wid);
//    if (!w) continue;
//
//    // Only send what the client asked for.
//    if (local[i].want_configure) {
//      if ((w->event_mask & (1u << 17)) && w->owner_fd > 0) {
//        send_ConfigureNotify(fd, seq0, wid, w->x, w->y, w->w, w->h, 0);
//      }
//    }
//
//    if (local[i].want_expose) {
//      if (w->mapped && (w->event_mask & (1u << 15)) && w->owner_fd > 0) {
//        send_Expose(fd, seq0, wid, 0, 0, w->w, w->h, 0);
//      }
//    }
//  }
//}


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

static pthread_mutex_t g_atoms_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  g_atoms_once = PTHREAD_ONCE_INIT;
static atom_entry_t* g_atoms = NULL;
static size_t g_atoms_n = 0;
static size_t g_atoms_cap = 0;

// X11 core protocol defines a fixed set of predefined atoms with fixed numeric IDs.
// If we start allocating at 1 we will collide with those and confuse clients (e.g. XA_CARDINAL==6).
// XA_LAST_PREDEFINED is 68 in the core protocol; allocate dynamic atoms starting at 69.
static uint32_t g_next_atom = 69; // 0 is None; 1..68 are predefined

static void atoms_init_predefined_once(void)
{
  // Predefined atom names indexed by (atom_id - 1). Must be exactly 68 entries.
  static const char* const kPredef[68] = {
    "PRIMARY",
    "SECONDARY",
    "ARC",
    "ATOM",
    "BITMAP",
    "CARDINAL",
    "COLORMAP",
    "CURSOR",
    "CUT_BUFFER0",
    "CUT_BUFFER1",
    "CUT_BUFFER2",
    "CUT_BUFFER3",
    "CUT_BUFFER4",
    "CUT_BUFFER5",
    "CUT_BUFFER6",
    "CUT_BUFFER7",
    "DRAWABLE",
    "FONT",
    "INTEGER",
    "PIXMAP",
    "POINT",
    "RECTANGLE",
    "RESOURCE_MANAGER",
    "RGB_COLOR_MAP",
    "RGB_BEST_MAP",
    "RGB_BLUE_MAP",
    "RGB_DEFAULT_MAP",
    "RGB_GRAY_MAP",
    "RGB_GREEN_MAP",
    "RGB_RED_MAP",
    "STRING",
    "VISUALID",
    "WINDOW",
    "WM_COMMAND",
    "WM_HINTS",
    "WM_CLIENT_MACHINE",
    "WM_ICON_NAME",
    "WM_ICON_SIZE",
    "WM_NAME",
    "WM_NORMAL_HINTS",
    "WM_SIZE_HINTS",
    "WM_ZOOM_HINTS",
    "MIN_SPACE",
    "NORM_SPACE",
    "MAX_SPACE",
    "END_SPACE",
    "SUPERSCRIPT_X",
    "SUPERSCRIPT_Y",
    "SUBSCRIPT_X",
    "SUBSCRIPT_Y",
    "UNDERLINE_POSITION",
    "UNDERLINE_THICKNESS",
    "STRIKEOUT_ASCENT",
    "STRIKEOUT_DESCENT",
    "ITALIC_ANGLE",
    "X_HEIGHT",
    "QUAD_WIDTH",
    "WEIGHT",
    "POINT_SIZE",
    "RESOLUTION",
    "COPYRIGHT",
    "NOTICE",
    "FONT_NAME",
    "FAMILY_NAME",
    "FULL_NAME",
    "CAP_HEIGHT",
    "WM_CLASS",
    "WM_TRANSIENT_FOR",
  };

  // Allocate backing array once and populate 1..68.
  g_atoms_cap = 128;
  g_atoms = (atom_entry_t*)calloc(g_atoms_cap, sizeof(*g_atoms));
  if (!g_atoms) {
    g_atoms_cap = 0;
    g_atoms_n = 0;
    // Leave g_next_atom at 69; later allocations will fail if we can't grow.
    return;
  }

  for (uint32_t i = 0; i < 68; i++) {
    const char* name = kPredef[i];
    const size_t len = strlen(name);
    char* s = (char*)malloc(len + 1);
    if (!s) continue;
    memcpy(s, name, len);
    s[len] = 0;
    g_atoms[g_atoms_n++] = (atom_entry_t){ .atom = (i + 1), .name = s, .len = len };
  }
}

static uint32_t atoms_intern(const char* name, size_t len, bool only_if_exists)
{
  pthread_once(&g_atoms_once, atoms_init_predefined_once);
  pthread_mutex_lock(&g_atoms_mu);

  for (size_t i = 0; i < g_atoms_n; i++) {
    if (g_atoms[i].len == len && memcmp(g_atoms[i].name, name, len) == 0) {
      uint32_t a = g_atoms[i].atom;
      pthread_mutex_unlock(&g_atoms_mu);
      return a;
    }
  }

  if (only_if_exists) {
    pthread_mutex_unlock(&g_atoms_mu);
    return 0;
  }

  if (g_atoms_n == g_atoms_cap) {
    size_t new_cap = (g_atoms_cap == 0) ? 32 : (g_atoms_cap * 2);
    atom_entry_t* p = (atom_entry_t*)realloc(g_atoms, new_cap * sizeof(*p));
    if (!p) {
      pthread_mutex_unlock(&g_atoms_mu);
      return 0;
    }
    g_atoms = p;
    g_atoms_cap = new_cap;
  }

  char* s = (char*)malloc(len + 1);
  if (!s) {
    pthread_mutex_unlock(&g_atoms_mu);
    return 0;
  }
  memcpy(s, name, len);
  s[len] = 0;

  uint32_t atom = g_next_atom++;
  g_atoms[g_atoms_n++] = (atom_entry_t){ .atom = atom, .name = s, .len = len };

  pthread_mutex_unlock(&g_atoms_mu);
  return atom;
}

static const char* atoms_name(uint32_t atom, size_t* out_len)
{
  pthread_once(&g_atoms_once, atoms_init_predefined_once);
  pthread_mutex_lock(&g_atoms_mu);
  for (size_t i = 0; i < g_atoms_n; i++) {
    if (g_atoms[i].atom == atom) {
      const char* s = g_atoms[i].name;
      if (out_len) *out_len = g_atoms[i].len;
      pthread_mutex_unlock(&g_atoms_mu);
      return s;
    }
  }
  pthread_mutex_unlock(&g_atoms_mu);
  if (out_len) *out_len = 0;
  return NULL;
}


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

static void handle_InternAtom(int fd, uint16_t seq, const uint8_t* payload, size_t remain, bool only_if_exists)
{
  // Request body after 4-byte header:
  //   CARD16 name_len
  //   CARD16 pad
  //   name bytes padded to 4
  if (remain < 4) return;

  const uint16_t name_len = (uint16_t)(payload[0] | ((uint16_t)payload[1] << 8));
  const size_t avail = (remain > 4u) ? (remain - 4u) : 0u;
  const size_t n = ((size_t)name_len < avail) ? (size_t)name_len : avail;

  uint32_t atom = atoms_intern((const char*)(payload + 4), n, only_if_exists);

//  uint8_t rep[32];
//  x11_reply32_le(rep, seq, 0);
//  rep[8]  = (uint8_t)(atom & 0xFF);
//  rep[9]  = (uint8_t)((atom >> 8) & 0xFF);
//  rep[10] = (uint8_t)((atom >> 16) & 0xFF);
//  rep[11] = (uint8_t)((atom >> 24) & 0xFF);
//  
//#ifndef NDEBUG
//  fprintf(stderr,
//          "[SwiftX11] xproto: REPLY op=handle_InternAtom seq=%u bytes=%zu length_words=%u\n",
//          (unsigned)seq,
//          (size_t)sizeof(rep),
//          (unsigned)rd32(rep + 4));
//  dbg_check_reply_total("InternAtom", seq, 32, rep);
//#endif
//  
//  (void)x11_send_all(fd, rep, sizeof(rep));
  
  (void)x11_proto_bridge_send_intern_atom_reply(seq, atom);
}

static void handle_GetAtomName(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
  (void)fd;
  if (remain < 4) return;

  uint32_t atom = rd32(payload + 0);

  size_t name_len_sz = 0;
  const char* name = atoms_name(atom, &name_len_sz);
  if (!name) { name = ""; name_len_sz = 0; }

  uint16_t name_len = (name_len_sz > 65535u) ? 65535u : (uint16_t)name_len_sz;

  (void)x11_proto_bridge_send_get_atom_name_reply(seq, name, name_len);
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


// Helper: apply value list updates for window (only event mask for now)
static void apply_value_list_updates_for_window(x11_win_t* w, uint32_t vmask, const uint8_t* vp, size_t vrem)
{
  if (!w) return;

  // Only care about CWEventMask (bit 11) for now.
  if (!(vmask & (1u << 11))) return;

  uint32_t cur_mask = w->event_mask;
  for (uint32_t bit = 0; bit < 32; bit++) {
    if (!(vmask & (1u << bit))) continue;
    if (vrem < 4) break;
    uint32_t val = rd32(vp);
    if (bit == 11) cur_mask = val; // CWEventMask
    vp += 4;
    vrem -= 4;
  }
  w->event_mask = cur_mask;
}


// MapSubwindows (major = 9) — maps all children (and descendants) of a window
static void handle_MapSubwindows(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
  if (remain < 4) return;
  const uint32_t parent = rd32(payload + 0);
#if !defined(NDEBUG) && SWIFTX11_TRACE
  size_t mapped_count = 0;
#endif

  // Map all descendants (not just direct children).
  // Repeat until no new windows are mapped.
  bool changed;
  do {
    changed = false;
    for (size_t i = 0; i < g_wins_n; i++) {
      x11_win_t* ch = &g_wins[i];

      // Already mapped? skip.
      if (ch->mapped) continue;

      // Map if its parent is the target parent OR if its parent is already mapped
      // as a descendant of the target.
      if (ch->parent == parent) {
        ch->mapped = 1;
        x11_proto_bridge_window_set_mapped(ch->xid, 1);
        changed = true;
      } else {
        x11_win_t* p = win_find(ch->parent);
        if (p && p->mapped) {
          // Only map if this parent is within the subtree rooted at `parent`.
          // We conservatively require that the parent itself is mapped AND either
          // parent==parent or the parent chain ultimately reaches `parent`.
          // Since we are iterating until convergence and only start by mapping
          // direct children of `parent`, this is safe.
          ch->mapped = 1;
          x11_proto_bridge_window_set_mapped(ch->xid, 1);
          changed = true;
        }
      }

      if (ch->mapped) {
#if !defined(NDEBUG) && SWIFTX11_TRACE
        mapped_count++;
#endif
        // enqueue to shim
        enqueue_map_window(ch->xid);
        x11_proto_bridge_window_set_mapped(ch->xid, 1);
        
        if (ch->dirty) {
          ch->dirty = 0;
          enqueue_damage_window(ch->xid);
        }
        
        //}
        if (ch->event_mask & (1u << 15)) { // ExposureMask
          //send_Expose(fd, seq, ch->xid, 0, 0, ch->w, ch->h, 0);
          x11_proto_bridge_queue_notify(ch->xid, 0 /*want_cfg*/, 1 /*want_exp*/);
        }
      }
    }
  } while (changed);
#if !defined(NDEBUG) && SWIFTX11_TRACE
  fprintf(stderr,
          "[SwiftX11] xproto: MapSubwindows parent=0x%08X seq=%u mapped_descendants=%zu\n",
          (unsigned)parent, (unsigned)seq, mapped_count);
#endif
}

// DestroyWindow (major = 4)
static void handle_DestroyWindow(int fd, const uint8_t* payload, size_t remain)
{
  (void)fd;
  if (remain < 4) return;
  uint32_t wid = rd32(payload + 0);

  for (size_t i = 0; i < g_wins_n; i++) {
    if (g_wins[i].xid == wid) {

      x11_proto_bridge_window_erase(wid);
      // free framebuffer for this slot
      if (g_framebuffers[i].pixels) {
        free(g_framebuffers[i].pixels);
        g_framebuffers[i].pixels = NULL;
      }

      // swap-with-last, keeping framebuffers aligned
      size_t last = g_wins_n - 1;
      if (i != last) {
        g_wins[i] = g_wins[last];
        g_framebuffers[i] = g_framebuffers[last];
      }

      g_wins_n--;
      break;
    }
  }
  
  enqueue_destroy_window(wid);
}


// ConfigureWindow (major = 12)
static void handle_ConfigureWindow(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] entered andle_ConfigureWindow (old): remain=%lu\n",
        remain);
#endif
  // Body (after 4-byte header):
  //   CARD32 window
  //   CARD16 valueMask
  //   CARD16 pad
  //   LISTofCARD32 values (but X11 defines 16-bit fields; encoded as 32-bit in request stream)
  if (remain < 8) return;

  const uint32_t wid = rd32(payload + 0);
  const uint16_t vmask = rd16(payload + 4);

  x11_win_t* w = win_find(wid);
  if (!w) return;

  // Values follow as 32-bit units in the order of bits set in vmask.
  const uint8_t* vp = payload + 8;
  size_t vrem = remain - 8;

  const  int16_t old_x = w->x;
  const  int16_t old_y = w->y;
  const uint16_t old_w = w->w;
  const uint16_t old_h = w->h;

#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] handle_ConfigureWindow (old): xid=0x%08X %dx%d\n",
        (unsigned)wid, (int)w->w, (int)w->h);
#endif

  int16_t  new_x = w->x;
  int16_t  new_y = w->y;
  uint16_t new_w = w->w;
  uint16_t new_h = w->h;

  for (uint32_t bit = 0; bit < 16; bit++) {
    if (!(vmask & (1u << bit))) continue;
    if (vrem < 4) break;
    uint32_t val32 = rd32(vp);

    switch (bit) {
      case 0: new_x = (int16_t)val32; break; // X
      case 1: new_y = (int16_t)val32; break; // Y
      case 2: new_w = (uint16_t)val32; break; // Width
      case 3: new_h = (uint16_t)val32; break; // Height
      default: break; // ignore borderWidth/sibling/stackmode for now
    }

    vp += 4;
    vrem -= 4;
  }

  w->x = new_x;
  w->y = new_y;
  w->w = (new_w ? new_w : 1);
  w->h = (new_h ? new_h : 1);
  if (w->x != old_x || w->y != old_y || w->w != old_w || w->h != old_h) {
    x11_proto_bridge_window_set_geometry(wid, w->x, w->y, w->w, w->h);
  }
  
#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] handle_ConfigureWindow (new): xid=0x%08X %dx%d\n",
        (unsigned)wid, (int)w->w, (int)w->h);
#endif

  // Only resize (and clear) the framebuffer if the request actually specified
  // width/height AND the size changed.
  const int wants_resize = (vmask & ((1u << 2) | (1u << 3))) != 0;
  const int size_changed = (w->w != old_w) || (w->h != old_h);

  if (wants_resize && size_changed) {
    const ssize_t idx = win_index(wid);
    if (idx >= 0) {
      x11_fb_t* fb = &g_framebuffers[(size_t)idx];

      const uint32_t new_fb_w = (uint32_t)w->w;
      const uint32_t new_fb_h = (uint32_t)w->h;
      const size_t new_npx = (size_t)new_fb_w * (size_t)new_fb_h;

      // Allocate-first then swap, so we don't lose the old buffer if malloc fails.
      uint32_t* new_pixels = (uint32_t*)malloc(new_npx * sizeof(uint32_t));
      if (new_pixels) {
        // Initialize new buffer to white.
        for (size_t i = 0; i < new_npx; i++) new_pixels[i] = 0xFFFFFFFFu;

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

        enqueue_damage_window(wid);
      }
      // If allocation fails, keep the old fb intact.
    }
  }

  // enqueue to shim
  enqueue_configure_window(wid, w->x, w->y, w->w, w->h);

  // If mapped and client selected for Exposure, send another Expose.
  const int want_cfg = (w->event_mask & (1u<<17)) != 0;   // StructureNotifyMask
  const int want_exp = (w->event_mask & (1u<<15)) != 0;  // ExposureMask
  x11_proto_bridge_queue_notify(wid, want_cfg, want_exp);
  // if (w->mapped && (w->event_mask & (1u << 15))) { // ExposureMask = (1<<15)
  //   send_Expose(fd, seq, wid, 0, 0, w->w, w->h, 0);
  // }
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
// Minimal GC table (enough for xeyes bitmaps and simple fills)
// ----------------------------------------------------------------------------
typedef struct {
  uint32_t xid;
  uint32_t fg; // ARGB
  uint32_t bg; // ARGB
} x11_gc_t;

// NOTE: For bring-up we keep a tiny fixed-size table.
static x11_gc_t g_gcs[256];
static size_t   g_gcs_n = 0;

static ssize_t gc_index(uint32_t xid)
{
  for (size_t i = 0; i < g_gcs_n; i++) {
    if (g_gcs[i].xid == xid) return (ssize_t)i;
  }
  return -1;
}

static x11_gc_t* gc_find(uint32_t xid)
{
  const ssize_t idx = gc_index(xid);
  return (idx >= 0) ? &g_gcs[(size_t)idx] : NULL;
}

static x11_gc_t* gc_alloc_or_get(uint32_t xid)
{
  if (xid == 0) return NULL;
  x11_gc_t* g = gc_find(xid);
  if (g) return g;
  if (g_gcs_n >= (sizeof(g_gcs) / sizeof(g_gcs[0]))) return NULL;
  g = &g_gcs[g_gcs_n++];
  g->xid = xid;
  // Default GC colors: X11 defaults are implementation-defined; for bring-up
  // we pick black fg and white bg so 1bpp masks look correct.
  g->fg = 0xFF000000u;
  g->bg = 0xFFFFFFFFu;
  return g;
}

static void gc_free(uint32_t xid)
{
  const ssize_t idx = gc_index(xid);
  if (idx < 0) return;
  const size_t last = g_gcs_n - 1;
  if ((size_t)idx != last) {
    g_gcs[(size_t)idx] = g_gcs[last];
  }
  g_gcs_n--;
}

// CreateGC (major = 55) -- no reply
static void handle_CreateGC(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
  (void)fd;
  (void)seq;

  // Body after 4-byte header:
  //   CARD32 gc
  //   CARD32 drawable
  //   CARD32 valueMask
  //   LISTofCARD32 values
  if (remain < 12) return;

  const uint32_t gc_xid    = rd32(payload + 0);
  const uint32_t drawable  = rd32(payload + 4);
  const uint32_t vmask     = rd32(payload + 8);

  x11_gc_t* g = gc_alloc_or_get(gc_xid);
  if (!g) return;

  // Parse values in order of bits set in vmask.
  // We only care about Foreground (bit 2) and Background (bit 3).
  const uint8_t* vp = payload + 12;
  size_t vrem = remain - 12;

  uint32_t new_fg = g->fg;
  uint32_t new_bg = g->bg;

  for (uint32_t bit = 0; bit < 32; bit++) {
    if (!(vmask & (1u << bit))) continue;
    if (vrem < 4) break;
    uint32_t val = rd32(vp);

    // X11 GC components (core):
    //   2 = GCForeground, 3 = GCBackground
    if (bit == 2) {
      // Pixel value; we don't have colormaps, but xeyes generally uses 0/1
      // for monochrome bitmaps. Map 0->black, nonzero->white is NOT correct
      // in general; instead, we keep raw pixel in low 24 bits as RGB.
      // For bring-up, treat 0 as black, 1 as white.
      if (val == 0) new_fg = 0xFF000000u;
      else if (val == 1) new_fg = 0xFFFFFFFFu;
      else new_fg = 0xFF000000u | (val & 0x00FFFFFFu);
    } else if (bit == 3) {
      if (val == 0) new_bg = 0xFF000000u;
      else if (val == 1) new_bg = 0xFFFFFFFFu;
      else new_bg = 0xFF000000u | (val & 0x00FFFFFFu);
    }

    vp += 4;
    vrem -= 4;
  }

  g->fg = new_fg;
  g->bg = new_bg;

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] CreateGC: gc=0x%08X drawable=0x%08X vmask=0x%08X fg=0x%08X bg=0x%08X (gcs_n=%zu)\n",
          (unsigned)gc_xid,
          (unsigned)drawable,
          (unsigned)vmask,
          (unsigned)g->fg,
          (unsigned)g->bg,
          g_gcs_n);
#endif
}

// ChangeGC (major = 56) -- no reply
static void handle_ChangeGC(const uint8_t* payload, size_t remain)
{
  // Body after 4-byte header:
  //   CARD32 gc
  //   CARD32 valueMask
  //   LISTofCARD32 values
  if (remain < 8) return;

  const uint32_t gc_xid = rd32(payload + 0);
  const uint32_t vmask  = rd32(payload + 4);

  x11_gc_t* g = gc_find(gc_xid);
  if (!g) return;

  const uint8_t* vp = payload + 8;
  size_t vrem = remain - 8;

  uint32_t new_fg = g->fg;
  uint32_t new_bg = g->bg;

  for (uint32_t bit = 0; bit < 32; bit++) {
    if (!(vmask & (1u << bit))) continue;
    if (vrem < 4) break;
    uint32_t val = rd32(vp);

    // X11 GC components (core):
    //   2 = GCForeground, 3 = GCBackground
    if (bit == 2) {
      if (val == 0) new_fg = 0xFF000000u;
      else if (val == 1) new_fg = 0xFFFFFFFFu;
      else new_fg = 0xFF000000u | (val & 0x00FFFFFFu);
    } else if (bit == 3) {
      if (val == 0) new_bg = 0xFF000000u;
      else if (val == 1) new_bg = 0xFFFFFFFFu;
      else new_bg = 0xFF000000u | (val & 0x00FFFFFFu);
    }

    vp += 4;
    vrem -= 4;
  }

  g->fg = new_fg;
  g->bg = new_bg;

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] ChangeGC: gc=0x%08X vmask=0x%08X fg=0x%08X bg=0x%08X\n",
          (unsigned)gc_xid,
          (unsigned)vmask,
          (unsigned)g->fg,
          (unsigned)g->bg);
#endif
}

// FreeGC (major = 60) -- no reply
static void handle_FreeGC(const uint8_t* payload, size_t remain)
{
  // Body after 4-byte header:
  //   CARD32 gc
  if (remain < 4) return;

  const uint32_t gc = rd32(payload + 0);

#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] FreeGC: gc=0x%08X (before gcs_n=%zu)\n", (unsigned)gc, g_gcs_n);
#endif

  gc_free(gc);

#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] FreeGC: gc=0x%08X (after gcs_n=%zu)\n", (unsigned)gc, g_gcs_n);
#endif
}

// create window (major = 1)
//static void handle_CreateWindow(uint8_t depth, const uint8_t* payload, size_t remain)
//{
//  // CreateWindow request body (after 4-byte header) begins with:
//  // 4: wid
//  // 4: parent
//  // 2: x
//  // 2: y
//  // 2: width
//  // 2: height
//  // 2: borderWidth
//  // 2: class
//  // 4: visual
//  // 4: valueMask
//  // then value-list (optional)
//  if (remain < 28) return;
//
//  uint32_t wid    = rd32(payload + 0);
//  uint32_t parent = rd32(payload + 4);
//  int16_t  x      = (int16_t)rd16(payload + 8);
//  int16_t  y      = (int16_t)rd16(payload + 10);
//  uint16_t wpx    = rd16(payload + 12);
//  uint16_t hpx    = rd16(payload + 14);
//  uint32_t vmask  = rd32(payload + 24);
//
//  // Create or overwrite (idempotent-ish for now), but always resolve an index so
//  // g_wins[] and g_framebuffers[] stay aligned.
//  ssize_t idx = win_index(wid);
//  if (idx < 0) idx = win_add(wid);
//  if (idx < 0) return;
//
//  x11_win_t* w = &g_wins[(size_t)idx];
//
//  w->parent      = parent;
//  w->x           = x; w->y = y;
//  w->w           = (wpx ? wpx : 1);
//  w->h           = (hpx ? hpx : 1);
//  w->mapped      = 0;
//  w->dirty       = 0;
//  w->event_mask  = 0;
//  w->presentable = 0;
//  w->owner_fd    = g_current_client_fd;
//
//  // Initialize (or re-initialize) framebuffer for this window at the SAME index.
//  x11_fb_t* fb = &g_framebuffers[(size_t)idx];
//
//  if (fb->pixels) {
//    free(fb->pixels);
//    fb->pixels = NULL;
//  }
//
//  fb->width  = (uint32_t)w->w;
//  fb->height = (uint32_t)w->h;
//
//  fb->pixels = (uint32_t*)malloc((size_t)fb->width * (size_t)fb->height * sizeof(uint32_t));
//  if (fb->pixels) {
//    const size_t npx = (size_t)fb->width * (size_t)fb->height;
//    for (size_t i = 0; i < npx; i++) fb->pixels[i] = 0xFFFFFFFFu;
//    w->dirty = 1;
//  } else {
//    fb->width = 0;
//    fb->height = 0;
//  }
//  
//  // If valueMask includes CWEventMask (bit 11) we should read it from value-list.
//  // valueMask bits are defined by X11; CWEventMask = (1<<11).
//  // value-list starts immediately after the fixed portion (28 bytes).
//  if (vmask & (1u << 11)) {
//    // value-list is 32-bit items in the order of bits set.
//    // For Phase A: we only care about CWEventMask, so we can scan in-order.
//    const uint8_t* vp = payload + 28;
//    size_t vrem = remain - 28;
//    uint32_t cur_mask = 0;
//
//    for (uint32_t bit = 0; bit < 32; bit++) {
//      if (!(vmask & (1u << bit))) continue;
//      if (vrem < 4) break;
//      uint32_t val = rd32(vp);
//      if (bit == 11) cur_mask = val; // CWEventMask
//      vp += 4; vrem -= 4;
//    }
//    w->event_mask = cur_mask;
//  }
//
//  x11_proto_bridge_window_upsert(wid, parent, x, y, w->w, w->h, w->event_mask, g_current_client_fd);
//  // Mirror initial lifecycle state into WindowTable.
//  x11_proto_bridge_window_set_mapped(wid, 0);
//  x11_proto_bridge_window_set_presentable(wid, 0);
//
//  // If we allocated/cleared the FB, we consider it "dirty" until map+presentable flushes it.
//  if (w->dirty) {
//    x11_proto_bridge_window_mark_dirty(wid);
//  }
//  
//  // enqueue to shim
//  enqueue_create_window(wid, parent, w->x, w->y, w->w, w->h, w->event_mask);
//
//#if !defined(NDEBUG) && SWIFTX11_TRACE
//  fprintf(stderr,
//          "[SwiftX11] xproto: CreateWindow wid=0x%08X parent=0x%08X vmask=0x%08X event_mask=0x%08X\n",
//          (unsigned)wid, (unsigned)parent, (unsigned)vmask, (unsigned)w->event_mask);
//#endif
//
//  (void)depth;
//}




// get window attributes (major = 3)
static void handle_GetWindowAttributes(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
  if (remain < 4) return;
  const uint32_t wid = rd32(payload + 0);

  x11_win_t* w = win_find(wid);

  // GetWindowAttributes reply is 44 bytes total.
  // The "length" field in the 32-byte reply header is the number of 4-byte units
  // *after* the first 32 bytes, so (44-32)/4 = 3.
  uint8_t rep[44];
  memset(rep, 0, sizeof(rep));

  rep[0] = 1; // Reply
  rep[1] = 0; // backing-store = NotUseful
  wr16_le(rep + 2, seq);
  wr32_le(rep + 4, 3); // extra 4-byte units after the first 32 bytes

  // visual (CARD32)
  wr32_le(rep + 8, X11_ROOT_VIS);
  // class (CARD16) InputOutput=1
  wr16_le(rep + 12, 1);

  // bit-gravity / win-gravity
  rep[14] = 0; // Forget
  rep[15] = 0; // Unmap

  // backing-planes / backing-pixel
  wr32_le(rep + 16, 0);
  wr32_le(rep + 20, 0);

  // save-under / map-is-installed / map-state / override-redirect
  rep[24] = 0; // saveUnder
  rep[25] = 1; // mapIsInstalled (true)
  rep[26] = (w && w->mapped) ? 2 : 0; // mapState: Viewable=2, Unmapped=0
  rep[27] = 0; // overrideRedirect

  // colormap
  wr32_le(rep + 28, 0x00000020u); // root defaultColormap we advertised in Setup

  // all-event-masks / your-event-mask
  wr32_le(rep + 32, w ? w->event_mask : 0);
  wr32_le(rep + 36, w ? w->event_mask : 0);

  // do-not-propagate-mask (CARD16) + pad (CARD16)
  wr16_le(rep + 40, 0);
  wr16_le(rep + 42, 0);

#if !defined(NDEBUG) && SWIFTX11_TRACE
  {
    uint32_t rep_len_words = rd32(rep + 4); // "length" field in reply header
    fprintf(stderr,
            "[SwiftX11] xproto: GetWindowAttributes wid=0x%08X seq=%u send_bytes=%zu rep.length=%u mapState=%u event_mask=0x%08X\n",
            (unsigned)wid,
            (unsigned)seq,
            (size_t)sizeof(rep),
            (unsigned)rep_len_words,
            (unsigned)rep[26],
            (unsigned)(w ? w->event_mask : 0));
    dbg_check_reply_total("GetWindowAttributes", seq, sizeof(rep), rep);
  }
#endif
  
  (void)x11_send_all(fd, rep, sizeof(rep));
}


static inline float norm360(float deg)
{
  float d = fmodf(deg, 360.0f);
  if (d < 0.0f) d += 360.0f;
  return d;
}

// X11: angle1 is start angle, angle2 is extent, both in degrees*64.
// Angles are measured CCW from the +X axis (“3 o’clock”).
// We compute theta_deg in that same convention.
static inline int angle_in_arc(float theta_deg, float start_deg, float extent_deg)
{
  theta_deg = norm360(theta_deg);
  start_deg = norm360(start_deg);

  if (extent_deg == 0.0f) return 0;

  if (extent_deg > 0.0f) {
    float delta = norm360(theta_deg - start_deg);   // CCW distance from start to theta
    return delta <= extent_deg;
  } else {
    float delta = norm360(start_deg - theta_deg);   // CW distance from start to theta
    return delta <= (-extent_deg);
  }
}

// Helper to resolve a drawable to a writable pixel buffer and size.
// Returns 1 on success, 0 on failure.
static int resolve_drawable_pixels_rw(uint32_t drawable,
                                     uint32_t **outPixels,
                                     uint32_t *outW,
                                     uint32_t *outH,
                                     bool *out_is_window)
{
  if (outPixels) *outPixels = NULL;
  if (outW) *outW = 0;
  if (outH) *outH = 0;
  if (out_is_window) *out_is_window = false;

  // Window drawable
  x11_win_t *w = win_find(drawable);
  if (w) {
    const ssize_t idx = win_index(drawable);
    if (idx < 0) return 0;
    x11_fb_t *fb = &g_framebuffers[(size_t)idx];
    if (!fb->pixels || fb->width == 0 || fb->height == 0) return 0;
    if (outPixels) *outPixels = fb->pixels;
    if (outW) *outW = fb->width;
    if (outH) *outH = fb->height;
    if (out_is_window) *out_is_window = true;
    return 1;
  }

  // Pixmap drawable
  const ssize_t pidx = pix_index(drawable);
  if (pidx < 0) return 0;

  x11_pixmap_t *pm = &g_pixmaps[(size_t)pidx];
  if (pm->width == 0 || pm->height == 0) return 0;

  // IMPORTANT: depth-1 pixmaps are packed bits in pm->bits, not 32bpp pixels.
  if (pm->depth == 1) return 0;

  if (!pm->pixels) return 0;

  if (outPixels) *outPixels = pm->pixels;
  if (outW) *outW = pm->width;
  if (outH) *outH = pm->height;
  if (out_is_window) *out_is_window = false;
  return 1;
}


// CreatePixmap (major = 53) -- no reply
static void handle_CreatePixmap(uint8_t depth, const uint8_t* payload, size_t remain)
{
  if (remain < 12) return;

  const uint32_t pid = rd32(payload + 0);
  // const uint32_t drawable = rd32(payload + 4); // unused for bring-up
  const uint16_t wpx = rd16(payload + 8);
  const uint16_t hpx = rd16(payload + 10);

  if (pid == 0) return;

  ssize_t idx = pix_alloc(pid);
  if (idx < 0) return;

  x11_pixmap_t *p = &g_pixmaps[(size_t)idx];

  p->depth  = depth;
  p->width  = (wpx ? wpx : 1);
  p->height = (hpx ? hpx : 1);

  // Reset any previous storage.
  free(p->pixels);
  free(p->bits);
  p->pixels = NULL;
  p->bits = NULL;
  p->stride_bytes = 0;

  if (p->depth == 1) {
    // Packed bitmap, scanlines padded to 32 bits.
    p->stride_bytes = (uint32_t)(((p->width + 31u) & ~31u) >> 3); // /8
    const size_t nbytes = (size_t)p->stride_bytes * (size_t)p->height;

    p->bits = (uint8_t*)malloc(nbytes);
    if (!p->bits) {
      p->width = p->height = 0;
      p->stride_bytes = 0;
      return;
    }

    // Initialize to 0 bits (background/off)
    memset(p->bits, 0, nbytes);
  } else {
    // 32-bit pixels.
    const size_t npx = (size_t)p->width * (size_t)p->height;
    p->pixels = (uint32_t*)malloc(npx * sizeof(uint32_t));
    if (!p->pixels) {
      p->width = p->height = 0;
      return;
    }

    // White background
    for (size_t i = 0; i < npx; i++) p->pixels[i] = 0xFFFFFFFFu;
  }
}


// FreePixmap (major = 54) -- no reply
static void handle_FreePixmap(const uint8_t* payload, size_t remain)
{
  if (remain < 4) return;
  const uint32_t pid = rd32(payload + 0);
  if (pid == 0) return;
  pix_free(pid);
}


// CopyArea (major = 62) -- no reply
static void handle_CopyArea(const uint8_t* payload, size_t remain)
{
  if (remain < 24) return;

  const uint32_t src = rd32(payload + 0);
  const uint32_t dst = rd32(payload + 4);
  const uint32_t gc  = rd32(payload + 8);

  const int16_t srcX = (int16_t)rd16(payload + 12);
  const int16_t srcY = (int16_t)rd16(payload + 14);
  const int16_t dstX = (int16_t)rd16(payload + 16);
  const int16_t dstY = (int16_t)rd16(payload + 18);

  const uint16_t wpx = rd16(payload + 20);
  const uint16_t hpx = rd16(payload + 22);

#ifndef NDEBUG
  fprintf(stderr,
        "[SwiftX11] CopyArea: src=0x%08X dst=0x%08X srcXY=(%d,%d) dstXY=(%d,%d) wh=(%u,%u) remain=%zu\n",
        (unsigned)src, (unsigned)dst,
        (int)srcX, (int)srcY,
        (int)dstX, (int)dstY,
        (unsigned)wpx, (unsigned)hpx,
        remain);
#endif
#ifndef NDEBUG
  {
    x11_gc_t* g = gc_find(gc);
    fprintf(stderr,
            "[SwiftX11] CopyArea: gc=0x%08X gc_found=%d fg=0x%08X bg=0x%08X\n",
            (unsigned)gc,
            g ? 1 : 0,
            (unsigned)(g ? g->fg : 0),
            (unsigned)(g ? g->bg : 0));
  }
#endif
  if (wpx == 0 || hpx == 0) return;

  // Resolve src buffer (window fb or pixmap)
  const uint32_t *srcPixels = NULL;
  int srcW = 0, srcH = 0;
  {
    x11_win_t *sw = win_find(src);
    if (sw) {
      ssize_t sidx = win_index(src);
      if (sidx < 0) return;
      x11_fb_t *sfb = &g_framebuffers[(size_t)sidx];
      if (!sfb->pixels) return;
      srcPixels = sfb->pixels;
      srcW = (int)sfb->width;
      srcH = (int)sfb->height;
    } else {
      ssize_t pidx = pix_index(src);
      if (pidx < 0) return;
      x11_pixmap_t *pm = &g_pixmaps[(size_t)pidx];
      if (!pm->pixels) return;
      srcPixels = pm->pixels;
      srcW = (int)pm->width;
      srcH = (int)pm->height;
    }
  }
#ifndef NDEBUG
  fprintf(stderr,
        "[SwiftX11] CopyArea: src_kind=%s srcWH=(%d,%d)\n",
        win_find(src) ? "window" : "pixmap",
        srcW, srcH);
#endif
  // Resolve dst buffer (window fb or pixmap)
  uint32_t *dstPixels = NULL;
  uint32_t dstW = 0, dstH = 0;
  bool dst_is_window = false;
  if (!resolve_drawable_pixels_rw(dst, &dstPixels, &dstW, &dstH, &dst_is_window)) return;
#ifndef NDEBUG
  fprintf(stderr,
        "[SwiftX11] CopyArea: dst_kind=%s dstWH=(%u,%u) -> enqueue_damage_window(0x%08X)\n",
        dst_is_window ? "window" : "pixmap",
        (unsigned)dstW, (unsigned)dstH, (unsigned)dst);
#endif
  
  // Minimal blit: per-pixel copy with clamp.
  for (int yy = 0; yy < (int)hpx; yy++) {
    int sy = (int)srcY + yy;
    int dy = (int)dstY + yy;
    if (sy < 0 || sy >= srcH) continue;
    if (dy < 0 || dy >= (int)dstH) continue;

    for (int xx = 0; xx < (int)wpx; xx++) {
      int sx = (int)srcX + xx;
      int dx = (int)dstX + xx;
      if (sx < 0 || sx >= srcW) continue;
      if (dx < 0 || dx >= (int)dstW) continue;

      dstPixels[(size_t)dy * (size_t)dstW + (size_t)dx] =
        srcPixels[(size_t)sy * (size_t)srcW + (size_t)sx];
    }
  }

  // Present it (only windows are presentable; pixmaps are presented when copied into a window)
  if (dst_is_window) {
    enqueue_damage_window(dst);
  }
}

// CopyPlane (major = 63) -- no reply
// Minimal implementation for bring-up (used by xeyes for 1bpp masks/bitmaps).
// Copies a 1-bit plane from src drawable to dst drawable using a simple fg/bg mapping.
// For now we ignore GC and use:
//   1-bit -> opaque black
//   0-bit -> opaque white
static void handle_CopyPlane(const uint8_t* payload, size_t remain)
{
  // Body after 4-byte header:
  //   CARD32 srcDrawable
  //   CARD32 dstDrawable
  //   CARD32 gc
  //   INT16  srcX
  //   INT16  srcY
  //   INT16  dstX
  //   INT16  dstY
  //   CARD16 width
  //   CARD16 height
  //   CARD32 bitPlane
  if (remain < 28) return;

  const uint32_t src = rd32(payload + 0);
  const uint32_t dst = rd32(payload + 4);
  const uint32_t gc  = rd32(payload + 8);

  const int16_t srcX = (int16_t)rd16(payload + 12);
  const int16_t srcY = (int16_t)rd16(payload + 14);
  const int16_t dstX = (int16_t)rd16(payload + 16);
  const int16_t dstY = (int16_t)rd16(payload + 18);

  const uint16_t wpx = rd16(payload + 20);
  const uint16_t hpx = rd16(payload + 22);

  const uint32_t bitPlane = rd32(payload + 24);

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] CopyPlane: src=0x%08X dst=0x%08X srcXY=(%d,%d) dstXY=(%d,%d) wh=(%u,%u) bitPlane=0x%08X remain=%zu\n",
          (unsigned)src, (unsigned)dst,
          (int)srcX, (int)srcY,
          (int)dstX, (int)dstY,
          (unsigned)wpx, (unsigned)hpx,
          (unsigned)bitPlane,
          remain);
#endif

  if (wpx == 0 || hpx == 0) return;

  // Minimal: only support plane 1 (the usual for depth-1 pixmaps).
  if (bitPlane != 1u) {
#ifndef NDEBUG
    fprintf(stderr, "[SwiftX11] CopyPlane: ignoring bitPlane=0x%08X (only 0x00000001 supported)\n",
            (unsigned)bitPlane);
#endif
    return;
  }

  // CopyPlane: X11 bitmapBitOrder handling.
  // We advertise LSBFirst in SetupSuccess, so keep that consistent here.
  const int BIT_ORDER_LSB_FIRST = 1;
  
  // Resolve src buffer (window fb or pixmap)
  const uint32_t *srcPixels = NULL;   // for window FB or non-1 pixmap
  const uint8_t  *srcBits   = NULL;   // for depth-1 pixmap
  int srcW = 0, srcH = 0;
  int src_pm_depth1 = 0;
  uint32_t src_stride_bytes = 0;

  {
    x11_win_t *sw = win_find(src);
    if (sw) {
      src_pm_depth1 = 0;
      const ssize_t sidx = win_index(src);
      if (sidx < 0) return;
      const x11_fb_t *sfb = &g_framebuffers[(size_t)sidx];
      if (!sfb->pixels) return;
      srcPixels = sfb->pixels;
      srcW = (int)sfb->width;
      srcH = (int)sfb->height;
    } else {
      const ssize_t pidx = pix_index(src);
      if (pidx < 0) return;
      const x11_pixmap_t *pm = &g_pixmaps[(size_t)pidx];
      if (pm->width == 0 || pm->height == 0) return;

      if (pm->depth == 1) {
        if (!pm->bits || pm->stride_bytes == 0) return;
        src_pm_depth1 = 1;
        srcBits = pm->bits;
        src_stride_bytes = pm->stride_bytes;
        srcW = (int)pm->width;
        srcH = (int)pm->height;
      } else {
        if (!pm->pixels) return;
        src_pm_depth1 = 0;
        srcPixels = pm->pixels;
        srcW = (int)pm->width;
        srcH = (int)pm->height;
      }
    }
  }

  // Resolve dst buffer (window fb or pixmap)
  uint32_t *dstPixels = NULL;
  int dstW = 0, dstH = 0;
  bool dst_is_window = false;
  uint8_t *dstBits = NULL;
  uint32_t dst_stride_bytes = 0;
  int dst_pm_depth1 = 0;
  {
    x11_win_t *dw = win_find(dst);
    if (dw) {
      const ssize_t didx = win_index(dst);
      if (didx < 0) return;
      x11_fb_t *dfb = &g_framebuffers[(size_t)didx];
      if (!dfb->pixels) return;
      dstPixels = dfb->pixels;
      dstW = (int)dfb->width;
      dstH = (int)dfb->height;
      dst_is_window = true;
    } else {
      const ssize_t pidx = pix_index(dst);
      if (pidx < 0) return;
      x11_pixmap_t *pm = &g_pixmaps[(size_t)pidx];
      if (pm->width == 0 || pm->height == 0) return;

      dstW = (int)pm->width;
      dstH = (int)pm->height;
      dst_is_window = false;

      if (pm->depth == 1) {
        // depth-1 destination: packed bits
        if (!pm->bits || pm->stride_bytes == 0) return;
        dst_pm_depth1 = 1;
        dstBits = pm->bits;
        dst_stride_bytes = pm->stride_bytes;
        dstPixels = NULL;
      } else {
        // 32bpp destination
        if (!pm->pixels) return;
        dst_pm_depth1 = 0;
        dstPixels = pm->pixels;
        dstBits = NULL;
        dst_stride_bytes = 0;
      }
    }
  }

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] CopyPlane: src_kind=%s srcWH=(%d,%d) dst_kind=%s dstWH=(%d,%d)\n",
          win_find(src) ? "window" : "pixmap", srcW, srcH,
          dst_is_window ? "window" : "pixmap", dstW, dstH);
#endif

  // GC mapping (minimal): use GC fg/bg if available.
  uint32_t fg = 0xFF000000u; // default black
  uint32_t bg = 0xFFFFFFFFu; // default white
  {
    x11_gc_t* g = gc_find(gc);
    if (g) {
      fg = g->fg;
      bg = g->bg;
    }
#ifndef NDEBUG
  fprintf(stderr,
    "[SwiftX11] CopyPlane: src=0x%08X(%s) dst=0x%08X(%s) src_depth1=%d fg=0x%08X bg=0x%08X\n",
    (unsigned)src, win_find(src) ? "win" : "pix",
    (unsigned)dst, win_find(dst) ? "win" : "pix",
    src_pm_depth1,
    (unsigned)fg, (unsigned)bg);
#endif
  }

  // Interpret source as 1-bit using our current storage convention:
  // PutImage(depth=1) expands bits into black/white pixels.
  size_t on_px = 0;
  for (int yy = 0; yy < (int)hpx; yy++) {
    int sy = (int)srcY + yy;
    int dy = (int)dstY + yy;
    if (sy < 0 || sy >= srcH) continue;
    if (dy < 0 || dy >= dstH) continue;

    for (int xx = 0; xx < (int)wpx; xx++) {
      int sx = (int)srcX + xx;
      int dx = (int)dstX + xx;
      if (sx < 0 || sx >= srcW) continue;
      if (dx < 0 || dx >= dstW) continue;

      int on = 0;

      if (src_pm_depth1) {
        const size_t byte_index =
          (size_t)sy * (size_t)src_stride_bytes + ((size_t)sx >> 3);

        const int bit_in_byte = BIT_ORDER_LSB_FIRST ? (sx & 7) : (7 - (sx & 7));
        on = (srcBits[byte_index] >> bit_in_byte) & 1;
      } else {
        const uint32_t sp =
          srcPixels[(size_t)sy * (size_t)srcW + (size_t)sx];
        on = (sp != 0xFFFFFFFFu);
      }
      
      if (on) on_px++;

      if (dst_pm_depth1) {
        // Store 1-bit result into packed bitmap (set/clear a bit).
        const size_t dbyte_index =
          (size_t)dy * (size_t)dst_stride_bytes + ((size_t)dx >> 3);
        const int dbit_in_byte = BIT_ORDER_LSB_FIRST ? (dx & 7) : (7 - (dx & 7));
        const uint8_t dmask = (uint8_t)(1u << dbit_in_byte);

        if (on) dstBits[dbyte_index] |= dmask;
        else    dstBits[dbyte_index] &= (uint8_t)~dmask;
      } else {
        // 32bpp destination
        dstPixels[(size_t)dy * (size_t)dstW + (size_t)dx] = on ? fg : bg;
      }
    }
  }
#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] CopyPlane: done on_px=%zu of %u dst_kind=%s\n",
          on_px,
          (unsigned)((uint32_t)wpx * (uint32_t)hpx),
          dst_is_window ? "window" : "pixmap");
#endif
  
  if (dst_is_window) {
    enqueue_damage_window(dst);
  }
}


// PolyArc (major = 68) -- no reply
// Minimal stroke implementation (xeyes uses this for eye outlines).
// Draws a ~1px outline of each ellipse/arc using GC foreground.
// Angle clipping is honored for non-full arcs.
static void handle_PolyArc(int fd, uint16_t seq,
                           const uint8_t* payload, size_t remain)
{
  (void)fd;
  (void)seq;

  // Body after 4-byte header:
  //   CARD32 drawable
  //   CARD32 gc
  //   LISTofxArc arcs (each 12 bytes)
  if (remain < 8) return;

  const uint32_t drawable = rd32(payload + 0);
  const uint32_t gc_id    = rd32(payload + 4);

#ifndef NDEBUG
  {
    const size_t list_bytes = (remain >= 8u) ? (remain - 8u) : 0u;
    const size_t narcs = list_bytes / 12u;
    x11_gc_t* g = gc_find(gc_id);
    fprintf(stderr,
            "[SwiftX11] PolyArc: drawable=0x%08X gc=0x%08X gc_found=%d fg=0x%08X bg=0x%08X narcs=%zu remain=%zu\n",
            (unsigned)drawable,
            (unsigned)gc_id,
            g ? 1 : 0,
            (unsigned)(g ? g->fg : 0),
            (unsigned)(g ? g->bg : 0),
            narcs,
            remain);
  }
#endif

  // Resolve destination buffer (window fb OR pixmap)
  uint32_t *dstPixels = NULL;
  uint32_t dstW = 0, dstH = 0;
  bool dst_is_window = false;
  if (!resolve_drawable_pixels_rw(drawable, &dstPixels, &dstW, &dstH, &dst_is_window)) return;

  const size_t list_bytes = remain - 8u;
  const size_t narcs = list_bytes / 12u;
  if (narcs == 0) return;

  // GC foreground (default black)
  uint32_t fg_color = 0xFF000000u;
  {
    x11_gc_t* g = gc_find(gc_id);
    if (g) fg_color = g->fg;
  }

  const uint8_t* arcs = payload + 8;

  for (size_t ai = 0; ai < narcs; ai++) {
    const uint8_t* ap = arcs + ai * 12u;

    // xArc is: INT16 x, INT16 y, CARD16 width, CARD16 height, INT16 angle1, INT16 angle2
    const int16_t  ax = (int16_t)rd16(ap + 0);
    const int16_t  ay = (int16_t)rd16(ap + 2);
    const uint16_t aw = rd16(ap + 4);
    const uint16_t ah = rd16(ap + 6);
    const int16_t  a1 = (int16_t)rd16(ap + 8);
    const int16_t  a2 = (int16_t)rd16(ap + 10);

    if (aw == 0 || ah == 0) continue;

    // X11 angles are in 1/64 degrees
    const float start_deg  = (float)a1 / 64.0f;
    const float extent_deg = (float)a2 / 64.0f;

    const float abs_extent = fabsf(extent_deg);
    const int full_ellipse = (abs_extent >= (360.0f - (1.0f / 64.0f)));

    // Ellipse center/radii
    const float rx = (float)aw * 0.5f;
    const float ry = (float)ah * 0.5f;
    if (rx <= 0.0f || ry <= 0.0f) continue;

    const float cx = (float)ax + rx;
    const float cy = (float)ay + ry;

    // Clamp bounding box
    int x0 = ax;
    int y0 = ay;
    int x1 = ax + (int)aw; // exclusive
    int y1 = ay + (int)ah; // exclusive
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)dstW)  x1 = (int)dstW;
    if (y1 > (int)dstH)  y1 = (int)dstH;
    if (x0 >= x1 || y0 >= y1) continue;

    // A thin band around the ellipse boundary.
    // Use epsilon in normalized space so thickness is ~1 pixel.
    const float eps = fmaxf(1.0f / fmaxf(rx, 1.0f), 1.0f / fmaxf(ry, 1.0f));

    for (int py = y0; py < y1; py++) {
      const float ny = ((float)py + 0.5f - cy) / ry;

      for (int px = x0; px < x1; px++) {
        const float nx = ((float)px + 0.5f - cx) / rx;
        const float d2 = nx*nx + ny*ny;

        // Keep only pixels near boundary
        const float dist = fabsf(d2 - 1.0f);
        if (dist > eps) continue;

        if (!full_ellipse) {
          const float dx = (float)px + 0.5f - cx;
          const float dy = (float)py + 0.5f - cy;
          float theta = atan2f(-dy, dx) * (180.0f / (float)M_PI);
          theta = norm360(theta);
          if (!angle_in_arc(theta, start_deg, extent_deg)) continue;
        }

        dstPixels[(size_t)py * (size_t)dstW + (size_t)px] = fg_color;
      }
    }
  }

  if (dst_is_window) {
    enqueue_damage_window(drawable);
  }
}


// PolyFillRectangle (major = 70)
static void handle_PolyFillRectangle(int fd, uint16_t seq,
                                     const uint8_t* payload, size_t remain)
{
  // Body after 4-byte header:
  //   CARD32 drawable
  //   CARD32 gc
  //   LISTofxRectangle rectangles (each 8 bytes)
  if (remain < 8) return;

  uint32_t drawable = rd32(payload + 0);
  uint32_t gc_id    = rd32(payload + 4);

#ifndef NDEBUG
  size_t dbg_written = 0;
  {
    const size_t list_bytes = (remain >= 8u) ? (remain - 8u) : 0u;
    const size_t nrects = list_bytes / 8u;
    x11_gc_t* g = gc_find(gc_id);
    fprintf(stderr,
            "[SwiftX11] PolyFillRectangle: drawable=0x%08X gc=0x%08X gc_found=%d fg=0x%08X bg=0x%08X nrects=%zu remain=%zu\n",
            (unsigned)drawable,
            (unsigned)gc_id,
            g ? 1 : 0,
            (unsigned)(g ? g->fg : 0),
            (unsigned)(g ? g->bg : 0),
            nrects,
            remain);
  }
#endif

  // Resolve destination buffer (window fb OR pixmap)
  uint32_t *dstPixels = NULL;
  uint32_t dstW = 0, dstH = 0;
  bool dst_is_window = false;
  if (!resolve_drawable_pixels_rw(drawable, &dstPixels, &dstW, &dstH, &dst_is_window)) return;

  // GC foreground (default black)
  uint32_t fg_color = 0xFF000000u;
  {
    x11_gc_t* g = gc_find(gc_id);
    if (g) fg_color = g->fg;
  }

  // Each rectangle is 8 bytes: x, y, width, height
  const uint8_t* rects = payload + 8;
  size_t nrects = (remain - 8) / 8;

  for (size_t ri = 0; ri < nrects; ri++) {
    int16_t x = (int16_t)rd16(rects + ri*8 + 0);
    int16_t y = (int16_t)rd16(rects + ri*8 + 2);
    uint16_t w_px = rd16(rects + ri*8 + 4);
    uint16_t h_px = rd16(rects + ri*8 + 6);

    for (int yy = 0; yy < h_px; yy++) {
      int row = y + yy;
      if (row < 0 || (uint32_t)row >= dstH) continue;
      for (int xx = 0; xx < w_px; xx++) {
        int col = x + xx;
        if (col < 0 || (uint32_t)col >= dstW) continue;
        const size_t idxp = (size_t)row * (size_t)dstW + (size_t)col;
        if (dstPixels[idxp] != fg_color) {
          dstPixels[idxp] = fg_color;
#ifndef NDEBUG
          dbg_written++;
#endif
        }
      }
    }
  }

#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] PolyFillRectangle: wrote_pixels=%zu (fg=0x%08X)\n",
          dbg_written, (unsigned)fg_color);
#endif

  // Notify UI shim only if the drawable is a window.
  if (dst_is_window) {
    enqueue_damage_window(drawable);
  }
}


// PolyFillArc (major = 71) -- no reply
static void handle_PolyFillArc(int fd, uint16_t seq,
                               const uint8_t* payload, size_t remain)
{
  (void)fd;
  (void)seq;

  // Body after 4-byte header:
  //   CARD32 drawable
  //   CARD32 gc
  //   LISTofxArc arcs (each 12 bytes)
  if (remain < 8) return;

  const uint32_t drawable = rd32(payload + 0);
  const uint32_t gc_id    = rd32(payload + 4);
  (void)gc_id;

#ifndef NDEBUG
  size_t dbg_written = 0;
  size_t dbg_inside = 0;
  size_t dbg_angle_reject = 0;
  {
    const size_t list_bytes = (remain >= 8u) ? (remain - 8u) : 0u;
    const size_t narcs = list_bytes / 12u;
    x11_gc_t* g = gc_find(gc_id);
    fprintf(stderr,
            "[SwiftX11] PolyFillArc: drawable=0x%08X gc=0x%08X gc_found=%d fg=0x%08X bg=0x%08X narcs=%zu remain=%zu\n",
            (unsigned)drawable,
            (unsigned)gc_id,
            g ? 1 : 0,
            (unsigned)(g ? g->fg : 0),
            (unsigned)(g ? g->bg : 0),
            narcs,
            remain);
  }
#endif

  // Resolve destination buffer (window fb OR pixmap)
  uint32_t *dstPixels = NULL;
  uint32_t dstW = 0, dstH = 0;
  bool dst_is_window = false;
  if (!resolve_drawable_pixels_rw(drawable, &dstPixels, &dstW, &dstH, &dst_is_window)) return;

  const size_t list_bytes = remain - 8u;
  const size_t narcs = list_bytes / 12u;
  if (narcs == 0) return;

  // GC foreground (default black)
  uint32_t fg_color = 0xFF000000u;
  {
    x11_gc_t* g = gc_find(gc_id);
    if (g) fg_color = g->fg;
  }

  const uint8_t* arcs = payload + 8;

  for (size_t ai = 0; ai < narcs; ai++) {
    const uint8_t* ap = arcs + ai * 12u;

    // xArc is: INT16 x, INT16 y, CARD16 width, CARD16 height, INT16 angle1, INT16 angle2
    const int16_t  ax = (int16_t)rd16(ap + 0);
    const int16_t  ay = (int16_t)rd16(ap + 2);
    const uint16_t aw = rd16(ap + 4);
    const uint16_t ah = rd16(ap + 6);
    const int16_t  a1 = (int16_t)rd16(ap + 8);
    const int16_t  a2 = (int16_t)rd16(ap + 10);
#ifndef NDEBUG
    fprintf(stderr,
            "[SwiftX11]   Arc[%zu]: xy=(%d,%d) wh=(%u,%u) a1=%d a2=%d\n",
            ai,
            (int)ax, (int)ay,
            (unsigned)aw, (unsigned)ah,
            (int)a1, (int)a2);
#endif
    if (aw == 0 || ah == 0) continue;

    // X11 angles are in 1/64 degrees
    const float start_deg  = (float)a1 / 64.0f;
    const float extent_deg = (float)a2 / 64.0f;

    // Treat ~full-circle extents as “fill entire ellipse”
    const float abs_extent = fabsf(extent_deg);
    // X11 uses degrees*64. Treat anything within 1/64 degree of a full circle as full.
    const int full_ellipse = (abs_extent >= (360.0f - (1.0f / 64.0f)));

    // Ellipse center/radii (float for coverage & angle math)
    const float rx = (float)aw * 0.5f;
    const float ry = (float)ah * 0.5f;
    if (rx <= 0.0f || ry <= 0.0f) continue;

    const float cx = (float)ax + rx;
    const float cy = (float)ay + ry;

    // Clamp bounding box to framebuffer
    int x0 = ax;
    int y0 = ay;
    int x1 = ax + (int)aw; // exclusive
    int y1 = ay + (int)ah; // exclusive

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)dstW)  x1 = (int)dstW;
    if (y1 > (int)dstH) y1 = (int)dstH;

    if (x0 >= x1 || y0 >= y1) continue;

#ifndef NDEBUG
    fprintf(stderr,
            "[SwiftX11]   Arc[%zu] bbox=(%d,%d)..(%d,%d) fb=%ux%u full=%d start=%.2f extent=%.2f\n",
            ai, x0, y0, x1, y1, (unsigned)dstW, (unsigned)dstH,
            full_ellipse, (double)start_deg, (double)extent_deg);
#endif
#ifndef NDEBUG
    {
      int sx = (int)cx;
      int sy = (int)cy;
      if (sx >= 0 && sy >= 0 && sx < (int)dstW && sy < (int)dstH) {
        uint32_t sp = dstPixels[(size_t)sy * (size_t)dstW + (size_t)sx];
        fprintf(stderr, "[SwiftX11]   Arc[%zu] sample@center (%d,%d) before=0x%08X\n",
                ai, sx, sy, (unsigned)sp);
      }
    }
#endif

    // Rasterize: test ellipse + optional angle wedge
    for (int py = y0; py < y1; py++) {
      // sample at pixel center
      const float yf = ((float)py + 0.5f - cy) / ry;

      for (int px = x0; px < x1; px++) {
        const float xf = ((float)px + 0.5f - cx) / rx;

        // inside ellipse?
        const float d2 = xf*xf + yf*yf;
        if (d2 > 1.0f) continue;
#ifndef NDEBUG
        dbg_inside++;
#endif

        if (!full_ellipse) {
          // Compute angle in X11 convention:
          // +x is 0 degrees; CCW positive.
          // Since framebuffer Y grows downward, use -(dy) to convert to math coords.
          const float dx = (float)px + 0.5f - cx;
          const float dy = (float)py + 0.5f - cy;

          float theta = atan2f(-dy, dx) * (180.0f / (float)M_PI);
          theta = norm360(theta);

          if (!angle_in_arc(theta, start_deg, extent_deg)) {
#ifndef NDEBUG
            dbg_angle_reject++;
#endif
            continue;
          }
        }

        const size_t idxp = (size_t)py * (size_t)dstW + (size_t)px;
#ifndef NDEBUG
        if (dstPixels[idxp] != fg_color) dbg_written++;
#endif
        dstPixels[idxp] = fg_color;
      }
    }
  }

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] PolyFillArc: changed_pixels=%zu inside_ellipse=%zu angle_reject=%zu (fg=0x%08X)\n",
          dbg_written, dbg_inside, dbg_angle_reject, (unsigned)fg_color);
#endif

  if (dst_is_window) {
    enqueue_damage_window(drawable);
  }
}


// PutImage (major = 72) -- no reply
// Minimal ZPixmap support (enough for many simple apps like xeyes).
// Supports both common cases:
//   - depth 24 with 24bpp packed BGR (3 bytes/pixel), scanlines padded to 32 bits
//   - depth 24/32 with 32bpp (4 bytes/pixel), scanlines padded to 32 bits
// PutImage (major = 72) -- no reply
// Supports the minimum needed for xeyes and similar clients:
//   - format=1 (XYPixmap) with depth=1 (1bpp)  [this is what xeyes uses for its stipple/bitmap]
//   - format=2 (ZPixmap) with depth=24/32 (best-effort)
//
// Notes:
//  - We advertise LSBFirst + bitmapScanlinePad=32 in SetupSuccess, so many clients will follow that.
//  - leftPad is honored for XYPixmap/1bpp.
static void handle_PutImage(uint8_t format, const uint8_t* payload, size_t remain)
{
  // Body after 4-byte request header:
  //   CARD32 drawable
  //   CARD32 gc
  //   CARD16 width
  //   CARD16 height
  //   INT16  dstX
  //   INT16  dstY
  //   CARD8  leftPad
  //   CARD8  depth
  //   CARD8  pad0
  //   CARD8  pad1
  //   LISTofBYTE data
  if (remain < 20) return;

  const uint32_t drawable = rd32(payload + 0);
  const uint32_t gc       = rd32(payload + 4);

  const uint16_t width  = rd16(payload + 8);
  const uint16_t height = rd16(payload + 10);
  const int16_t  dst_x  = (int16_t)rd16(payload + 12);
  const int16_t  dst_y  = (int16_t)rd16(payload + 14);

  const uint8_t leftPad = payload[16];
  const uint8_t depth   = payload[17];

  if (width == 0 || height == 0) return;

  const uint8_t* src = payload + 20;
  const size_t src_len = remain - 20;

  // Resolve destination buffer: window framebuffer OR pixmap.
  uint32_t* dstPixels = NULL;
  uint32_t dstW = 0, dstH = 0;
  bool dst_is_window = false;
  x11_pixmap_t* dst_pm = NULL;
  uint8_t*  dstBits = NULL;
  uint32_t  dstStrideBytes = 0;
  int       dst_pm_depth1 = 0;

  {
    x11_win_t* w = win_find(drawable);
    if (w) {
      const ssize_t idx = win_index(drawable);
      if (idx < 0) return;
      x11_fb_t* fb = &g_framebuffers[(size_t)idx];
      if (!fb->pixels || fb->width == 0 || fb->height == 0) return;
      dstPixels = fb->pixels;
      dstW = fb->width;
      dstH = fb->height;
      dst_is_window = true;
      dst_pm = NULL;   // optional but explicit
    } else {
      const ssize_t pidx = pix_index(drawable);
      if (pidx < 0) return;
      x11_pixmap_t* pm = &g_pixmaps[(size_t)pidx];
      if (pm->width == 0 || pm->height == 0) return;

      dstW = pm->width;
      dstH = pm->height;
      dst_is_window = false;
      dst_pm = pm;

      if (pm->depth == 1) {
        if (!pm->bits || pm->stride_bytes == 0) return;
        dst_pm_depth1 = 1;
        dstBits = pm->bits;
        dstStrideBytes = pm->stride_bytes;
        dstPixels = NULL;
      } else {
        if (!pm->pixels) return;
        dst_pm_depth1 = 0;
        dstPixels = pm->pixels;
        dstBits = NULL;
        dstStrideBytes = 0;
      }
    }
  }

#ifndef NDEBUG
  // Keep this fairly quiet by default; you can flip SWIFTX11_TRACE to 1.
  if (SWIFTX11_TRACE) {
    x11_gc_t* g = gc_find(gc);
    fprintf(stderr,
            "[SwiftX11] PutImage: format=%u drawable=0x%08X gc=0x%08X gc_found=%d dst_kind=%s w=%u h=%u dst=(%d,%d) leftPad=%u depth=%u src_len=%zu\n",
            (unsigned)format,
            (unsigned)drawable,
            (unsigned)gc,
            g ? 1 : 0,
            dst_is_window ? "window" : "pixmap",
            (unsigned)width,
            (unsigned)height,
            (int)dst_x,
            (int)dst_y,
            (unsigned)leftPad,
            (unsigned)depth,
            src_len);
  }
#endif

#ifndef NDEBUG
fprintf(stderr,
  "[SwiftX11] PutImage dst_kind=%s pm=%p pm_depth=%d depth1=%d dstStride=%u dstWH=%ux%u\n",
  dst_is_window ? "window" : "pixmap",
  (void*)dst_pm,
  dst_pm ? (int)dst_pm->depth : -1,
  dst_pm_depth1,
  (unsigned)dstStrideBytes,
  (unsigned)dstW, (unsigned)dstH);
#endif
  
  // -------------------------------------------------------------------------
  // Case A: XYPixmap, depth=1 (this is the key path for xeyes)
  // -------------------------------------------------------------------------
  // XYPixmap packs bits into scanlines; scanlines are padded to bitmapScanlinePad.
  // For depth=1 there is only one plane.
  //
  // We advertised:
  //   bitmapBitOrder   = LSBFirst
  //   bitmapScanlinePad= 32
  // so we assume bit0 is the leftmost pixel within each byte.
  // If a client uses MSBFirst anyway, flip the bit-indexing easily.
  const int BIT_ORDER_LSB_FIRST = 1; // matches SetupSuccess we send
  const uint32_t BITMAP_PAD = 32;    // matches SetupSuccess we send

  if (format == 1) {
    if (depth != 1) {
#ifndef NDEBUG
      if (SWIFTX11_TRACE) {
        fprintf(stderr, "[SwiftX11] PutImage: XYPixmap unsupported depth=%u (only depth=1 implemented)\n", (unsigned)depth);
      }
#endif
      return;
    }

    // Compute stride:
    // Bits per scanline = leftPad + width.
    // Round up to BITMAP_PAD bits, then convert to bytes.
    const uint32_t bits_per_line = (uint32_t)leftPad + (uint32_t)width;
    const uint32_t padded_bits   = (bits_per_line + (BITMAP_PAD - 1u)) & ~(BITMAP_PAD - 1u);
    const size_t stride_bytes    = (size_t)(padded_bits / 8u);

    // Total bytes = stride * height (depth=1 plane)
    if (height != 0 && stride_bytes > (SIZE_MAX / (size_t)height)) return;
    const size_t need = stride_bytes * (size_t)height;
    if (src_len < need) {
#ifndef NDEBUG
      fprintf(stderr, "[SwiftX11] PutImage: XYPixmap depth=1 not enough data src_len=%zu need=%zu (stride=%zu h=%u)\n",
              src_len, need, stride_bytes, (unsigned)height);
#endif
      return;
    }
#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] PutImage(XYPixmap d=1): drawable=0x%08X gc=0x%08X w=%u h=%u dst=(%d,%d) leftPad=%u stride=%zu need=%zu src_len=%zu\n",
          (unsigned)drawable,
          (unsigned)gc,
          (unsigned)width,
          (unsigned)height,
          (int)dst_x,
          (int)dst_y,
          (unsigned)leftPad,
          stride_bytes,
          need,
          src_len);
#endif
    
    // Use GC fg/bg if present; otherwise default to black/white.
    uint32_t on_color  = 0xFF000000u;
    uint32_t off_color = 0xFFFFFFFFu;
    if (!dst_is_window && dst_pm && dst_pm->depth == 1) {
      on_color  = 1u;
      off_color = 0u;
    } else {
      x11_gc_t* g = gc_find(gc);
      if (g) { 
        on_color = g->fg; 
        off_color = g->bg; 
      }
    }
    
    size_t on_bits = 0;
    for (uint32_t y = 0; y < (uint32_t)height; y++) {
      const int32_t dy = (int32_t)dst_y + (int32_t)y;
      if (dy < 0 || (uint32_t)dy >= dstH) continue;

      const uint8_t* srow = src + (size_t)y * stride_bytes;

      for (uint32_t x = 0; x < (uint32_t)width; x++) {
        const int32_t dx = (int32_t)dst_x + (int32_t)x;
        if (dx < 0 || (uint32_t)dx >= dstW) continue;

        // Bit index within the scanline:
        const uint32_t bit = (uint32_t)leftPad + x;
        const uint32_t byte_i = bit >> 3;
        const uint32_t bit_i  = bit & 7u;

        const uint8_t b = srow[byte_i];
        const uint32_t mask = BIT_ORDER_LSB_FIRST ? (1u << bit_i) : (1u << (7u - bit_i));
        const uint32_t on = (b & (uint8_t)mask) ? 1u : 0u;
        on_bits += (size_t)on;
        
        if (dst_pm_depth1) {
          // Store into packed depth-1 pixmap bits.
          // We advertise bitmapBitOrder = LSBFirst in SetupSuccess.
          const uint32_t dxu = (uint32_t)dx;
          const uint32_t dyu = (uint32_t)dy;

          const uint32_t byte_x = dxu >> 3;
          const uint32_t bit_x  = dxu & 7u;

          const uint32_t dbit = BIT_ORDER_LSB_FIRST ? bit_x : (7u - bit_x);
          const size_t dbyte =
            (size_t)dyu * (size_t)dstStrideBytes + (size_t)byte_x;          const uint8_t dmask = (uint8_t)(1u << dbit);

          if (on) dstBits[dbyte] |= dmask;
          else    dstBits[dbyte] &= (uint8_t)~dmask;
        } else {
          // Store into 32bpp window/pixmap pixels.
          dstPixels[(size_t)dy * (size_t)dstW + (size_t)dx] = on ? on_color : off_color;
        }
      }
    }

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] PutImage(XYPixmap d=1): wrote on_bits=%zu of %zu dst_kind=%s\n",
          on_bits,
          (size_t)width * (size_t)height,
          dst_is_window ? "window" : "pixmap");

  if (!dst_is_window && dst_pm && dst_pm->depth == 1) {
    // depth-1 pixmap: sample first byte of packed bits
    uint8_t b0 = (dst_pm->bits && dst_pm->stride_bytes && dst_pm->height) ? dst_pm->bits[0] : 0;
    fprintf(stderr, "[SwiftX11] PutImage: depth1 pixmap sample bits[0]=0x%02X\n", (unsigned)b0);
  } else if (dstPixels) {
    // window or depth>1 pixmap: 32-bit pixels
    const uint32_t p0 = dstPixels[0];
    fprintf(stderr, "[SwiftX11] PutImage: sample p0=0x%08X\n", (unsigned)p0);
  } else {
    fprintf(stderr, "[SwiftX11] PutImage: sample skipped (no dstPixels)\n");
  }
#endif
    
    // Damage: if drawable is a window, present it; if pixmap, don't.
    // The pixmap will be copied into a window via CopyArea, which will damage the window.
    if (dst_is_window) enqueue_damage_window(drawable);
    return;
  }

  // -------------------------------------------------------------------------
  // Case B: ZPixmap, depth=24/32 (best-effort)
  // -------------------------------------------------------------------------
  if (format != 2) {
#ifndef NDEBUG
    if (SWIFTX11_TRACE) fprintf(stderr, "[SwiftX11] PutImage: ignoring unsupported format=%u\n", (unsigned)format);
#endif
    return;
  }

  if (!(depth == 24 || depth == 32)) {
#ifndef NDEBUG
    if (SWIFTX11_TRACE) fprintf(stderr, "[SwiftX11] PutImage: ZPixmap unsupported depth=%u\n", (unsigned)depth);
#endif
    return;
  }

  // Determine which layout the client sent.
  // Many clients with depth=24 send packed 24bpp (3 bytes/pixel) with 32-bit padded scanlines.
  // Some send 32bpp (4 bytes/pixel).
  const size_t stride_24 = (size_t)(((uint32_t)width * 24u + 31u) / 32u) * 4u; // 24bpp padded to 32 bits
  const size_t stride_32 = (size_t)(((uint32_t)width * 32u + 31u) / 32u) * 4u; // 32bpp padded to 32 bits

  if (height != 0 && stride_24 > (SIZE_MAX / (size_t)height)) return;
  if (height != 0 && stride_32 > (SIZE_MAX / (size_t)height)) return;

  const size_t need_24 = stride_24 * (size_t)height;
  const size_t need_32 = stride_32 * (size_t)height;

  int use32 = 0;
  size_t stride = 0;

  // Prefer the interpretation that fits the provided buffer.
  // If both fit, prefer 32bpp.
  if (src_len >= need_32) {
    use32 = 1;
    stride = stride_32;
  } else if (src_len >= need_24) {
    use32 = 0;
    stride = stride_24;
  } else {
#ifndef NDEBUG
    fprintf(stderr,
            "[SwiftX11] PutImage: ZPixmap not enough data src_len=%zu need32=%zu need24=%zu\n",
            src_len, need_32, need_24);
#endif
    return;
  }

#ifndef NDEBUG
  if (SWIFTX11_TRACE) {
    fprintf(stderr, "[SwiftX11] PutImage: ZPixmap accepted layout=%s stride=%zu\n", use32 ? "32bpp" : "24bpp", stride);
  }
#endif

  // Copy into our destination pixels.
  // We treat incoming pixel order as little-endian B,G,R,(X).
  for (uint32_t y = 0; y < (uint32_t)height; y++) {
    const int32_t dy = (int32_t)dst_y + (int32_t)y;
    if (dy < 0 || (uint32_t)dy >= dstH) continue;

    const uint8_t* srow = src + (size_t)y * stride;

    for (uint32_t x = 0; x < (uint32_t)width; x++) {
      const int32_t dx = (int32_t)dst_x + (int32_t)x;
      if (dx < 0 || (uint32_t)dx >= dstW) continue;

      uint8_t b = 0, g = 0, r = 0;
      if (use32) {
        const size_t o = (size_t)x * 4u;
        b = srow[o + 0u];
        g = srow[o + 1u];
        r = srow[o + 2u];
      } else {
        const size_t o = (size_t)x * 3u;
        b = srow[o + 0u];
        g = srow[o + 1u];
        r = srow[o + 2u];
      }

      const uint32_t pix =
          (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | (0xFFu << 24);

      dstPixels[(size_t)dy * (size_t)dstW + (size_t)dx] = pix;
    }
  }

  // If this is a window, present. If it is a pixmap, let CopyArea present later.
  if (dst_is_window) enqueue_damage_window(drawable);
}


#ifndef NDEBUG
static void dbg_dump_req(const char* tag,
                         uint8_t major, uint8_t minor, uint16_t len_words,
                         const uint8_t* payload, size_t remain)
{
  fprintf(stderr,
          "[SwiftX11] xproto: DUMP %s major=%u minor=%u len_words=%u remain=%zu first16=",
          tag, (unsigned)major, (unsigned)minor, (unsigned)len_words, remain);
  size_t n = (remain < 16) ? remain : 16;
  for (size_t i = 0; i < n; i++) fprintf(stderr, "%02x ", (unsigned)payload[i]);
  fprintf(stderr, "\n");
}
#endif


// ClearArea (major = 61) -- no reply, but may generate Expose events if exposures==true
static void handle_ClearArea(int fd, uint16_t seq, uint8_t exposures,
                            const uint8_t* payload, size_t remain)
{
  if (remain < 12) return;

  const uint32_t wid = rd32(payload + 0);
  const int16_t  x   = (int16_t)rd16(payload + 4);
  const int16_t  y   = (int16_t)rd16(payload + 6);
  const uint16_t wpx = rd16(payload + 8);
  const uint16_t hpx = rd16(payload + 10);

  x11_win_t* w = win_find(wid);
  if (!w) return;

  const ssize_t idx = win_index(wid);
  if (idx < 0) return;

  x11_fb_t* fb = &g_framebuffers[(size_t)idx];
  if (!fb->pixels) return;

  // X11 semantics: width/height == 0 means “to the bottom/right edge”
  int x0 = (int)x;
  int y0 = (int)y;

  int x1 = (wpx == 0) ? (int)fb->width  : (x0 + (int)wpx);
  int y1 = (hpx == 0) ? (int)fb->height : (y0 + (int)hpx);

  // Clamp
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > (int)fb->width)  x1 = (int)fb->width;
  if (y1 > (int)fb->height) y1 = (int)fb->height;

  if (x0 < x1 && y0 < y1) {
    const uint32_t bg = 0xFFFFFFFFu; // opaque white background (minimal)
    for (int yy = y0; yy < y1; yy++) {
      uint32_t* row = fb->pixels + (size_t)yy * (size_t)fb->width;
      for (int xx = x0; xx < x1; xx++) {
        row[xx] = bg;
      }
    }

    // Mark window damaged (so your shim presents updated pixels)
    enqueue_damage_window(wid);
  }

  // If exposures==true and the client selected ExposureMask, send Expose
//  if (exposures && w->mapped && (w->event_mask & (1u << 15))) {
//    x11_proto_bridge_queue_notify(wid, 0 /*want_cfg*/, 1 /*want_exp*/);
//  }
  if (exposures && w->mapped && (w->event_mask & (1u << 15))) {
    // Expose wants unsigned coords; clamp to 0
    uint16_t ex = (x0 < 0) ? 0u : (uint16_t)x0;
    uint16_t ey = (y0 < 0) ? 0u : (uint16_t)y0;
    uint16_t ew = (uint16_t)(x1 - x0);
    uint16_t eh = (uint16_t)(y1 - y0);
    //send_Expose(fd, seq, wid, ex, ey, ew, eh, 0);
    x11_proto_bridge_queue_expose_rect(wid, ex, ey, ew, eh, 0);
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
        //case 1: // CreateWindow
        //  handle_CreateWindow(minor /*depth*/, payload, remain);
        //  break;
                    
        //case 3: // GetWindowAttributes
        //  handle_GetWindowAttributes(cfd, seq, payload, remain);
        //  break;
          
        case 4: // DestroyWindow
          handle_DestroyWindow(cfd, payload, remain);
          break;
          
          
        //case 9: // MapSubwindows
        //  handle_MapSubwindows(cfd, seq, payload, remain);
        //  break;
          
        case 12: // ConfigureWindow
          handle_ConfigureWindow(cfd, seq, payload, remain);
          break;
          
        case 53: // CreatePixmap (no reply)
          handle_CreatePixmap(minor /*depth*/, payload, remain);
          break;
          
        case 54: // FreePixmap (no reply)
          handle_FreePixmap(payload, remain);
          break;
          
        case 55: // CreateGC (no reply)
          handle_CreateGC(cfd, seq, payload, remain );
          break;
        case 56: // ChangeGC (no reply)
          handle_ChangeGC(payload, remain);
          break;
          
        case 60: // FreeGC (no reply)
          handle_FreeGC(payload, remain);
          break;
          
        case 61: // ClearArea (no reply; may generate Expose)
          handle_ClearArea(cfd, seq, minor /*exposures*/, payload, remain);
          break;
          
        case 62: // CopyArea (no reply)
          handle_CopyArea(payload, remain);
          break;
          
        case 63: // CopyPlane (no reply)
          handle_CopyPlane(payload, remain);
          break;
          
        case 68: // PolyArc (no reply)
          handle_PolyArc(cfd, seq, payload, remain);
          break;
          
        case 70: // PolyFillRectangle
          handle_PolyFillRectangle(cfd, seq, payload, remain);
          break;
          
        case 71: // PolyFillArc (no reply)
          handle_PolyFillArc(cfd, seq, payload, remain);
          break;
          
        case 72: // PutImage (no reply)
          handle_PutImage(minor /*format*/, payload, remain);
          break;
          
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



