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
#include <errno.h>
#ifndef NDEBUG
#include <pthread.h>
#endif
#include <sys/socket.h>   // send()
#include <sys/types.h>


#include <CoreGraphics/CoreGraphics.h>

#include "x11_requests.h"
#include "x11_setup.h"

// For SIGPIPE avoidance on macOS
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// -----------------------------------------------------------------------------
// helpers
// -----------------------------------------------------------------------------
static inline void wr16_le(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void wr32_le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFFu);
  p[1] = (uint8_t)((v >> 8) & 0xFFu);
  p[2] = (uint8_t)((v >> 16) & 0xFFu);
  p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

#ifndef NDEBUG
static size_t fb_count_nonwhite(const uint32_t* px, size_t npx) {
  size_t nw = 0;
  for (size_t i = 0; i < npx; i++) {
    if (px[i] != 0xFFFFFFFFu) nw++;
  }
  return nw;
}

static void fb_sample3(const uint32_t* px, uint32_t w, uint32_t h,
                       uint32_t* outL, uint32_t* outC, uint32_t* outR) {
  if (!px || w == 0 || h == 0) { *outL = *outC = *outR = 0; return; }
  const uint32_t y = h / 2;
  const uint32_t xL = w / 4;
  const uint32_t xC = w / 2;
  const uint32_t xR = (w * 3) / 4;
  *outL = px[(size_t)y * (size_t)w + (size_t)xL];
  *outC = px[(size_t)y * (size_t)w + (size_t)xC];
  *outR = px[(size_t)y * (size_t)w + (size_t)xR];
}
#endif

static void x11_get_virtual_desktop_px(uint16_t* outW, uint16_t* outH,
                                       uint16_t* outWmm, uint16_t* outHmm)
{
  if (outW) *outW = 800;
  if (outH) *outH = 600;
  if (outWmm) *outWmm = 270;
  if (outHmm) *outHmm = 203;

  CGDirectDisplayID displays[16];
  uint32_t count = 0;
  if (CGGetActiveDisplayList((uint32_t)(sizeof(displays)/sizeof(displays[0])),
                             displays, &count) != kCGErrorSuccess || count == 0) {
    return;
  }

  // Union display bounds in *device pixels*
  CGRect b0 = CGDisplayBounds(displays[0]);
  double minX = CGRectGetMinX(b0);
  double minY = CGRectGetMinY(b0);
  double maxX = CGRectGetMaxX(b0);
  double maxY = CGRectGetMaxY(b0);

  for (uint32_t i = 1; i < count; i++) {
    CGRect b = CGDisplayBounds(displays[i]);
    if (CGRectGetMinX(b) < minX) minX = CGRectGetMinX(b);
    if (CGRectGetMinY(b) < minY) minY = CGRectGetMinY(b);
    if (CGRectGetMaxX(b) > maxX) maxX = CGRectGetMaxX(b);
    if (CGRectGetMaxY(b) > maxY) maxY = CGRectGetMaxY(b);
  }

  double w = maxX - minX;
  double h = maxY - minY;
  if (w < 1) w = 1;
  if (h < 1) h = 1;

  // Clamp to uint16_t limits
  if (w > 65535.0) w = 65535.0;
  if (h > 65535.0) h = 65535.0;

  const uint16_t wpx = (uint16_t)w;
  const uint16_t hpx = (uint16_t)h;

  if (outW) *outW = wpx;
  if (outH) *outH = hpx;

  // Physical mm: use main display pixel->mm scale as an approximation for the union
  CGDirectDisplayID main = CGMainDisplayID();
  CGSize mm = CGDisplayScreenSize(main);                // mm for main display
  size_t mainW = CGDisplayPixelsWide(main);
  size_t mainH = CGDisplayPixelsHigh(main);

  if (mainW > 0 && mainH > 0 && mm.width > 0 && mm.height > 0) {
    const double mmPerPxX = mm.width  / (double)mainW;
    const double mmPerPxY = mm.height / (double)mainH;

    double wmm = (double)wpx * mmPerPxX;
    double hmm = (double)hpx * mmPerPxY;

    if (wmm < 1) wmm = 1;
    if (hmm < 1) hmm = 1;
    if (wmm > 65535.0) wmm = 65535.0;
    if (hmm > 65535.0) hmm = 65535.0;

    if (outWmm) *outWmm = (uint16_t)wmm;
    if (outHmm) *outHmm = (uint16_t)hmm;
  }
}

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
typedef struct {
  uint32_t  xid;
  int       owner_fd;
  uint32_t  width;
  uint32_t  height;
  uint32_t* pixels;
} x11_fb_slot_t;

static x11_fb_slot_t g_fb[256];
static size_t        g_fb_n = 0;


static ssize_t fb_index(uint32_t xid)
{
  for (size_t i = 0; i < g_fb_n; i++) {
    if (g_fb[i].xid == xid) return (ssize_t)i;
  }
  return -1;
}

static x11_fb_slot_t* fb_find(uint32_t xid)
{
  const ssize_t idx = fb_index(xid);
  return (idx >= 0) ? &g_fb[(size_t)idx] : NULL;
}

// Add a new window slot
// Returns the index on success, -1 on failure.
static ssize_t fb_add(uint32_t xid)
{
  if (g_fb_n >= (sizeof(g_fb)/sizeof(g_fb[0]))) return -1;

  const size_t idx = g_fb_n++;
  x11_fb_slot_t* w = &g_fb[idx];
  memset(w, 0, sizeof(*w));
  w->xid = xid;

  return (ssize_t)idx;
}

// -----------------------------
// C++ bridge helpers (implemented in C++)
// -----------------------------
extern uint32_t x11_cpp_list_descendants(uint32_t host, uint32_t* out, uint32_t cap);
extern int x11_cpp_get_window_geom(uint32_t xid,
                                   uint32_t* out_parent,
                                   int16_t* out_x,
                                   int16_t* out_y,
                                   uint16_t* out_w,
                                   uint16_t* out_h,
                                   int* out_mapped);
extern int x11_cpp_get_abs_pos_in_host(uint32_t host, uint32_t xid,
                                       int32_t* out_abs_x,
                                       int32_t* out_abs_y);

// -----------------------------
// Opaque blit child FB over host scratch
// -----------------------------
static void blit_child_over_host(uint32_t* hostPx, int hostW, int hostH,
                                 const uint32_t* childPx, int childW, int childH,
                                 int32_t dstX, int32_t dstY)
{
  if (!hostPx || !childPx) return;
  if (hostW <= 0 || hostH <= 0 || childW <= 0 || childH <= 0) return;

  // Host-space rect for child
  int32_t x0 = dstX;
  int32_t y0 = dstY;
  int32_t x1 = dstX + (int32_t)childW;
  int32_t y1 = dstY + (int32_t)childH;

  // Clip to host
  int32_t cx0 = (x0 < 0) ? 0 : x0;
  int32_t cy0 = (y0 < 0) ? 0 : y0;
  int32_t cx1 = (x1 > hostW) ? hostW : x1;
  int32_t cy1 = (y1 > hostH) ? hostH : y1;

  if (cx0 >= cx1 || cy0 >= cy1) return;

  // Source offset if clipped
  int32_t sx0 = cx0 - x0;
  int32_t sy0 = cy0 - y0;

  const int32_t copyW = cx1 - cx0;
  const int32_t copyH = cy1 - cy0;

  for (int32_t yy = 0; yy < copyH; yy++) {
    const uint32_t* srcRow = childPx
      + (size_t)(sy0 + yy) * (size_t)childW
      + (size_t)sx0;

    uint32_t* dstRow = hostPx
      + (size_t)(cy0 + yy) * (size_t)hostW
      + (size_t)cx0;

    memcpy(dstRow, srcRow, (size_t)copyW * sizeof(uint32_t));
  }
}

// -----------------------------
// x11_xproto_copy_window_bgra: subtree composite (rootless)
// -----------------------------
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
#endif

  const ssize_t host_idx = fb_index(xid);
  x11_fb_slot_t* host = (host_idx >= 0) ? &g_fb[(size_t)host_idx] : NULL;

  if (!host || !host->pixels || host->width == 0 || host->height == 0) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_bpr) *out_bpr = 0;
    return 0;
  }

  const int32_t w   = (int32_t)host->width;
  const int32_t h   = (int32_t)host->height;
  const int32_t bpr = w * 4;

  // validate needed size
  const int64_t needed64 = (int64_t)bpr * (int64_t)h;
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

  // size-only query
  if (!out_bytes || out_cap == 0) return 1;
  if (out_cap < needed) return 0;

  // Scratch composited buffer
  const size_t npx = (size_t)host->width * (size_t)host->height;
  const size_t bytes = npx * sizeof(uint32_t);

  uint32_t* scratch = (uint32_t*)malloc(bytes);
  if (!scratch) return 0;

  memcpy(scratch, host->pixels, bytes);

  // Composite mapped descendants onto scratch.
  // (Cap is a bring-up choice; increase if needed.)
  uint32_t kids[2048];
  const uint32_t nk = x11_cpp_list_descendants(xid, kids, (uint32_t)(sizeof(kids)/sizeof(kids[0])));

  
  for (uint32_t i = 0; i < nk; i++) {
    const uint32_t kid = kids[i];

    uint32_t parent = 0;
    int16_t relx = 0, rely = 0;
    uint16_t kw = 0, kh = 0;
    int mapped = 0;

    if (!x11_cpp_get_window_geom(kid, &parent, &relx, &rely, &kw, &kh, &mapped))
      continue;
    if (!mapped) continue;

    const ssize_t kidx = fb_index(kid);
    if (kidx < 0) continue;

    x11_fb_slot_t* kfb = &g_fb[(size_t)kidx];
    if (!kfb->pixels || kfb->width == 0 || kfb->height == 0) continue;

    int32_t absx = 0, absy = 0;
    if (!x11_cpp_get_abs_pos_in_host(xid, kid, &absx, &absy))
      continue;

#ifndef NDEBUG
    fprintf(stderr,
            "[COMPDBG] host=0x%08X kid=0x%08X mapped=%d geom=%ux%u fb=%ux%u abs=(%d,%d)\n",
            (unsigned)xid,
            (unsigned)kid,
            mapped,
            (unsigned)kw, (unsigned)kh,
            (unsigned)kfb->width, (unsigned)kfb->height,
            (int)absx, (int)absy);
#endif
    
    
    blit_child_over_host(
      scratch, (int)host->width, (int)host->height,
      (const uint32_t*)kfb->pixels, (int)kfb->width, (int)kfb->height,
      absx, absy
    );
  }

#ifndef NDEBUG
  // Optional: verify nonwhite in composited result (cheap sanity)
  size_t nonwhite = 0;
  for (size_t i = 0; i < npx; i++) if (scratch[i] != 0xFFFFFFFFu) nonwhite++;
  fprintf(stderr, "[SwiftX11] xproto_copy_window_bgra: COMPOSITE xid=0x%08X %dx%d nonwhite=%zu\n",
          (unsigned)xid, (int)w, (int)h, nonwhite);
#endif

  memcpy(out_bytes, (const void*)scratch, (size_t)needed);
  free(scratch);
  return 1;
}




#ifndef NDEBUG
// Record the xproto client thread so other threads can detect "not on xproto thread".
static pthread_t g_xproto_thread;
static int g_xproto_thread_valid = 0;

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

  const uint8_t* p = (const uint8_t*)buf;
  
#ifndef NDEBUG
  {
    const uint8_t* b = (const uint8_t*)buf;

    // alignment check
    if ((n % 4) != 0) {
      fprintf(stderr, "[SENDALL BADALIGN] n=%zu head=", n);
      for (size_t i = 0; i < 16 && i < n; i++) fprintf(stderr, "%02X", b[i]);
      fprintf(stderr, "\n");
      fflush(stderr);
    }

    if (n == 32) {
      uint8_t b0 = b[0];
      uint16_t seq = (uint16_t)(b[2] | ((uint16_t)b[3] << 8));
      uint32_t lenw = (uint32_t)(b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24));
      fprintf(stderr, "[SENDALL 32] b0=%u seq=%u lenw=%u (%u bytes) b1=%u\n",
              (unsigned)b0, (unsigned)seq, (unsigned)lenw, (unsigned)(lenw*4u), (unsigned)b[1]);
      fflush(stderr);
    } else {
      fprintf(stderr, "[SENDALL CHUNK] n=%zu head=", n);
      for (size_t i = 0; i < 16 && i < n; i++) fprintf(stderr, "%02X", b[i]);
      fprintf(stderr, "\n");
      fflush(stderr);
    }
  }
#endif
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

int x11_backend_fb_create_slot(uint32_t wid,
                               uint16_t wpx,
                               uint16_t hpx,
                               int owner_fd,
                               int* out_dirty)
{
  if (out_dirty) *out_dirty = 0;
  if (wid == 0) return 0;

  // Create or overwrite (idempotent-ish)
  ssize_t idx = fb_index(wid);
  if (idx < 0) idx = fb_add(wid);
  if (idx < 0) return 0;

  x11_fb_slot_t* w = &g_fb[(size_t)idx];

  // Mirror exactly what handle_CreateWindow used to do
  w->width       = (wpx ? wpx : 1);
  w->height      = (hpx ? hpx : 1);
  w->owner_fd    = owner_fd;

  if (w->pixels) {
    free(w->pixels);
    w->pixels = NULL;
  }

  w->pixels = (uint32_t*)malloc((size_t)w->width * (size_t)w->height * sizeof(uint32_t));
  if (w->pixels) {
    const size_t npx = (size_t)w->width * (size_t)w->height;
    for (size_t i = 0; i < npx; i++) w->pixels[i] = 0xFFFFFFFFu;
    if (out_dirty) *out_dirty = 1;
  } else {
    w->width = 0;
    w->height = 0;
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

  const ssize_t idx = fb_index(xid);
  if (idx < 0) return 0;

  x11_fb_slot_t* fb = &g_fb[(size_t)idx];
  if (!fb->pixels || fb->width == 0 || fb->height == 0) return 0;

  if (outPixels) *outPixels = fb->pixels;
  if (outW) *outW = fb->width;
  if (outH) *outH = fb->height;
  return 1;
}


// Backing store cleanup only (window framebuffer memory).
void x11_backend_fb_destroy(uint32_t wid)
{
  if (wid == 0) return;

  for (size_t i = 0; i < g_fb_n; i++) {
    if (g_fb[i].xid == wid) {
      free(g_fb[i].pixels);
      g_fb[i].pixels = NULL;

      size_t last = g_fb_n - 1;
      if (i != last) {
        g_fb[i] = g_fb[last];
      }
      g_fb_n--;
      return;
    }
  }
}

// ************ this entire block should be deleted once we have verified C++ ********
// ----------------------------------------------------------------------------
// Server-thread -> xproto-thread notify queue (to avoid cross-thread socket writes)
// ----------------------------------------------------------------------------



void x11_send_setup_failed_le(int fd, const char* reason)
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
void x11_send_setup_success_minimal_little_endian(int fd)
{
  // ---- Tunables / IDs
  const uint16_t proto_major = 11;
  const uint16_t proto_minor = 0;
  const uint32_t rid_base    = 0x10000000u;
  const uint32_t rid_mask    = 0x0FFFFFFFu;
  const uint32_t root_xid    = 0x00000001u;
  const uint32_t root_visid  = 0x00000021u;
  const uint32_t root_cmap   = 0x00000020u;

  uint16_t screen_w_px = 800, screen_h_px = 600;
  uint16_t screen_w_mm = 270, screen_h_mm = 203;
  x11_get_virtual_desktop_px(&screen_w_px, &screen_h_px, &screen_w_mm, &screen_h_mm);
  fprintf( stderr, "[DISPLAY] advertised screen_w_px = %d  screen_h_px = %d\n", screen_w_px, screen_h_px);
  
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
// Tiny Atom table (enough for InternAtom/GetAtomName)
// ----------------------------------------------------------------------------


void x11_backend_fb_resize_impl(uint32_t wid, uint16_t new_w, uint16_t new_h)
{
  if (wid == 0) return;
  if (new_w == 0) new_w = 1;
  if (new_h == 0) new_h = 1;

  x11_fb_slot_t* s = fb_find(wid);
  if (!s) {
#ifndef NDEBUG
    fprintf(stderr, "[FB_RESIZE] wid=0x%08X -> %ux%u (NO SLOT)\n",
            (unsigned)wid, (unsigned)new_w, (unsigned)new_h);
#endif
    return;
  }

  const uint32_t old_w = s->width;
  const uint32_t old_h = s->height;

  const uint32_t w = (uint32_t)new_w;
  const uint32_t h = (uint32_t)new_h;

#ifndef NDEBUG
  fprintf(stderr, "[FB_RESIZE] wid=0x%08X %ux%u -> %ux%u slot=%p pix=%p tid=%p\n",
          (unsigned)wid,
          (unsigned)old_w, (unsigned)old_h,
          (unsigned)new_w, (unsigned)new_h,
          (void*)s, (void*)s->pixels, (void*)pthread_self());
#endif

  if (w == old_w && h == old_h) {
#ifndef NDEBUG
    fprintf(stderr, "[FB_RESIZE] wid=0x%08X (NOOP same size)\n", (unsigned)wid);
#endif
    return;
  }

  const size_t new_npx = (size_t)w * (size_t)h;

#ifndef NDEBUG
  fprintf(stderr, "[FB_RESIZE] wid=0x%08X new_npx=%zu bytes=%zu\n",
          (unsigned)wid, new_npx, new_npx * sizeof(uint32_t));
#endif

  // Allocate-first; leave old buffer intact on failure.
  uint32_t* new_pixels = (uint32_t*)malloc(new_npx * sizeof(uint32_t));
  if (!new_pixels) {
#ifndef NDEBUG
    fprintf(stderr, "[FB_RESIZE] wid=0x%08X malloc FAIL bytes=%zu\n",
            (unsigned)wid, new_npx * sizeof(uint32_t));
#endif
    return;
  }

#ifndef NDEBUG
  fprintf(stderr, "[FB_RESIZE] wid=0x%08X malloc OK new_pixels=%p\n",
          (unsigned)wid, (void*)new_pixels);
#endif

  // Init white.
  for (size_t i = 0; i < new_npx; i++) new_pixels[i] = 0xFFFFFFFFu;

  // Preserve overlap.
  const uint32_t copy_w = (old_w < w) ? old_w : w;
  const uint32_t copy_h = (old_h < h) ? old_h : h;

#ifndef NDEBUG
  fprintf(stderr, "[FB_RESIZE] wid=0x%08X copy_w=%u copy_h=%u\n",
          (unsigned)wid, (unsigned)copy_w, (unsigned)copy_h);
#endif

  if (s->pixels && copy_w && copy_h) {
    for (uint32_t yy = 0; yy < copy_h; yy++) {
      const uint32_t* src_row = s->pixels + (size_t)yy * (size_t)old_w;
      uint32_t* dst_row       = new_pixels + (size_t)yy * (size_t)w;
      memcpy(dst_row, src_row, (size_t)copy_w * sizeof(uint32_t));
    }
  }

#ifndef NDEBUG
  fprintf(stderr, "[FB_RESIZE] wid=0x%08X free old_pixels=%p\n",
          (unsigned)wid, (void*)s->pixels);
#endif

  free(s->pixels);
  s->pixels = new_pixels;
  s->width  = w;
  s->height = h;
#ifndef NDEBUG
  fprintf(stderr, "[FB_RESIZE] wid=0x%08X DONE new=%ux%u pix=%p\n",
          (unsigned)wid, (unsigned)s->width, (unsigned)s->height, (void*)s->pixels);
#endif

}


int x11_xproto_get_fb_size(uint32_t xid, int32_t* out_w_px, int32_t* out_h_px)
{
  if (out_w_px) *out_w_px = 0;
  if (out_h_px) *out_h_px = 0;
  if (xid == 0) return 0;

  x11_fb_slot_t* s = fb_find(xid);
  if (!s) return 0;

  if (out_w_px) *out_w_px = (int32_t)s->width;
  if (out_h_px) *out_h_px = (int32_t)s->height;
  return 1;
}

#include "x11_backend_fb.h"

static x11_fb_event_fn g_obs = 0;
static void* g_obs_user = 0;

void x11_backend_fb_set_event_observer(x11_fb_event_fn fn, void* user) {
  g_obs = fn;
  g_obs_user = user;
}

void x11_backend_fb_resize_dbg(uint32_t wid, uint16_t new_w, uint16_t new_h,
                               const char* why, const char* file, int line)
{
  x11_backend_fb_resize_impl(wid, new_w, new_h);
  if (g_obs) g_obs(wid, new_w, new_h, "resize", why, file, line, g_obs_user);
}

int x11_backend_fb_create_slot_dbg(uint32_t wid, uint16_t wpx, uint16_t hpx,
                                  int owner_fd, int* out_dirty,
                                  const char* why, const char* file, int line)
{
  int ok = x11_backend_fb_create_slot(wid, wpx, hpx, owner_fd, out_dirty);
  if (ok && g_obs) {
    uint16_t fw = wpx ? wpx : 1;
    uint16_t fh = hpx ? hpx : 1;
    g_obs(wid, fw, fh, "create", why, file, line, g_obs_user);
  }
  return ok;
}
