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

  const ssize_t idx = fb_index(xid);
  x11_fb_slot_t* fb = (idx >= 0) ? &g_fb[(size_t)idx] : NULL;

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
// Tiny Atom table (enough for InternAtom/GetAtomName)
// ----------------------------------------------------------------------------

#include <string.h> // memcpy

void x11_backend_fb_resize(uint32_t wid, uint16_t new_w, uint16_t new_h)
{
  if (wid == 0) return;
  if (new_w == 0) new_w = 1;
  if (new_h == 0) new_h = 1;

  x11_fb_slot_t* s = fb_find(wid);
  if (!s) return;

  const uint32_t old_w = s->width;
  const uint32_t old_h = s->height;

  const uint32_t w = (uint32_t)new_w;
  const uint32_t h = (uint32_t)new_h;

  if (w == old_w && h == old_h) return;

  const size_t new_npx = (size_t)w * (size_t)h;

  // Allocate-first; leave old buffer intact on failure.
  uint32_t* new_pixels = (uint32_t*)malloc(new_npx * sizeof(uint32_t));
  if (!new_pixels) return;

  // Init white.
  for (size_t i = 0; i < new_npx; i++) new_pixels[i] = 0xFFFFFFFFu;

  // Preserve overlap.
  const uint32_t copy_w = (old_w < w) ? old_w : w;
  const uint32_t copy_h = (old_h < h) ? old_h : h;

  if (s->pixels && copy_w && copy_h) {
    for (uint32_t yy = 0; yy < copy_h; yy++) {
      const uint32_t* src_row = s->pixels + (size_t)yy * (size_t)old_w;
      uint32_t* dst_row       = new_pixels + (size_t)yy * (size_t)w;
      memcpy(dst_row, src_row, (size_t)copy_w * sizeof(uint32_t));
    }
  }

  free(s->pixels);
  s->pixels = new_pixels;
  s->width  = w;
  s->height = h;
}





// Copies current framebuffer bytes for xid into out_bytes.
// Returns 1 on success, 0 on failure.
// If out_bytes is NULL or out_cap too small, returns 0 but fills out_w/out_h/out_bpr.
//int x11_backend_copy_window_bgra(uint32_t xid,
//                                 uint8_t* out_bytes,
//                                 int32_t out_cap,
//                                 int32_t* out_w,
//                                 int32_t* out_h,
//                                 int32_t* out_bpr)
//{
//  const ssize_t idx = fb_index(xid);
//  x11_fb_slot_t* fb = (idx >= 0) ? &g_fb[(size_t)idx] : NULL;
//
//  if (!fb || !fb->pixels || fb->width == 0 || fb->height == 0) {
//    if (out_w) *out_w = 0;
//    if (out_h) *out_h = 0;
//    if (out_bpr) *out_bpr = 0;
//    return 0;
//  }
//
//  int32_t w = (int32_t)fb->width;
//  int32_t h = (int32_t)fb->height;
//  int32_t bpr = w * 4;
//  int64_t needed64 = (int64_t)bpr * (int64_t)h;
//  if (needed64 <= 0 || needed64 > INT32_MAX) return 0;
//  int32_t needed = (int32_t)needed64;
//
//  if (out_w) *out_w = w;
//  if (out_h) *out_h = h;
//  if (out_bpr) *out_bpr = bpr;
//
//  if (!out_bytes || out_cap < needed) return 0;
//
//#ifndef NDEBUG
//  {
//    // Sample a few pixels so we can confirm the server-drawn FB is what the UI copies.
//    const int32_t sx_c = w / 2;
//    const int32_t sy_c = h / 2;
//    const int32_t sx_l = w / 4;
//    const int32_t sx_r = (w * 3) / 4;
//    const int32_t sy_m = h / 2;
//
//    uint32_t sp_c = 0, sp_l = 0, sp_r = 0;
//    if (sx_c >= 0 && sy_c >= 0 && sx_c < w && sy_c < h) {
//      sp_c = fb->pixels[(size_t)sy_c * (size_t)fb->width + (size_t)sx_c];
//    }
//    if (sx_l >= 0 && sy_m >= 0 && sx_l < w && sy_m < h) {
//      sp_l = fb->pixels[(size_t)sy_m * (size_t)fb->width + (size_t)sx_l];
//    }
//    if (sx_r >= 0 && sy_m >= 0 && sx_r < w && sy_m < h) {
//      sp_r = fb->pixels[(size_t)sy_m * (size_t)fb->width + (size_t)sx_r];
//    }
//
//    // Count non-white pixels (very cheap sanity check for "did we draw anything?")
//    size_t nonwhite = 0;
//    const size_t npx = (size_t)fb->width * (size_t)fb->height;
//    for (size_t i = 0; i < npx; i++) {
//      if (fb->pixels[i] != 0xFFFFFFFFu) nonwhite++;
//    }
//
//    fprintf(stderr,
//            "[SwiftX11] copy_window_bgra: xid=0x%08X fb=%p %dx%d nonwhite=%zu sample L/C/R=0x%08X 0x%08X 0x%08X\n",
//            (unsigned)xid, (void*)fb->pixels, (int)w, (int)h,
//            nonwhite, (unsigned)sp_l, (unsigned)sp_c, (unsigned)sp_r);
//  }
//#endif
//
//  memcpy(out_bytes, (const void*)fb->pixels, (size_t)needed);
//  return 1;
//}



