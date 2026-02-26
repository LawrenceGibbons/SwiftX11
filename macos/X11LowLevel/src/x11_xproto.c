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
#include <math.h>  // add near top (needed for fabs)
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

static inline int x11_nearly_1px(double a, double b) {
  return fabs(a - b) < 1.0; // 1 unit tolerance
}

// Returns logical desktop size in X11 units ("points") to match Swift-side model.
// outW/outH are X11 units. (Signature keeps _px name for now.)
static void x11_get_virtual_desktop_px(uint16_t* outW, uint16_t* outH,
                                       uint16_t* outWmm, uint16_t* outHmm)
{
  // Safe defaults
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

  // Union bounds in X11 units (points)
  double minX_u = 0, minY_u = 0, maxX_u = 0, maxY_u = 0;
  int have = 0;

  for (uint32_t i = 0; i < count; i++) {
    const CGDirectDisplayID did = displays[i];
    CGRect b = CGDisplayBounds(did);

    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(did);
    double mode_w_u = 0, mode_h_u = 0, mode_w_px = 0, mode_h_px = 0;
    double scale_x = 1.0, scale_y = 1.0;

    if (mode) {
      mode_w_u  = (double)CGDisplayModeGetWidth(mode);
      mode_h_u  = (double)CGDisplayModeGetHeight(mode);
      mode_w_px = (double)CGDisplayModeGetPixelWidth(mode);
      mode_h_px = (double)CGDisplayModeGetPixelHeight(mode);
      if (mode_w_u > 0 && mode_w_px > 0) scale_x = mode_w_px / mode_w_u;
      if (mode_h_u > 0 && mode_h_px > 0) scale_y = mode_h_px / mode_h_u;
      if (scale_x <= 0.0) scale_x = 1.0;
      if (scale_y <= 0.0) scale_y = 1.0;
    }

    // Detect whether CGDisplayBounds is expressed in px or u (points).
    // We check width/height against mode pixelWidth vs mode width.
    const double bw = CGRectGetWidth(b);
    const double bh = CGRectGetHeight(b);

    int bounds_is_px = 0;
    int bounds_is_u  = 0;

    if (mode) {
      if (mode_w_px > 0 && x11_nearly_1px(bw, mode_w_px)) bounds_is_px = 1;
      if (mode_w_u  > 0 && x11_nearly_1px(bw, mode_w_u )) bounds_is_u  = 1;

      // If ambiguous, try height too
      if (!bounds_is_px && mode_h_px > 0 && x11_nearly_1px(bh, mode_h_px)) bounds_is_px = 1;
      if (!bounds_is_u  && mode_h_u  > 0 && x11_nearly_1px(bh, mode_h_u )) bounds_is_u  = 1;    }

    // Convert bounds to X11 units
    double x_u = CGRectGetMinX(b);
    double y_u = CGRectGetMinY(b);
    double w_u = bw;
    double h_u = bh;

    if (bounds_is_px && !bounds_is_u) {
      x_u /= scale_x;
      y_u /= scale_y;
      w_u /= scale_x;
      h_u /= scale_y;
    } else {
      // Treat as already in logical units.
      // (If neither detection hits, this is the safer choice for "points model".)
    }

    const double x1_u = x_u + w_u;
    const double y1_u = y_u + h_u;

    if (!have) {
      minX_u = x_u; minY_u = y_u; maxX_u = x1_u; maxY_u = y1_u;
      have = 1;
    } else {
      if (x_u  < minX_u) minX_u = x_u;
      if (y_u  < minY_u) minY_u = y_u;
      if (x1_u > maxX_u) maxX_u = x1_u;
      if (y1_u > maxY_u) maxY_u = y1_u;
    }

    if (mode) CFRelease(mode);
  }

  if (!have) return;

  double w_u = maxX_u - minX_u;
  double h_u = maxY_u - minY_u;
  if (w_u < 1.0) w_u = 1.0;
  if (h_u < 1.0) h_u = 1.0;

  if (w_u > 65535.0) w_u = 65535.0;
  if (h_u > 65535.0) h_u = 65535.0;

  const uint16_t wu16 = (uint16_t)(w_u + 0.5);
  const uint16_t hu16 = (uint16_t)(h_u + 0.5);

  if (outW) *outW = wu16;
  if (outH) *outH = hu16;

  // Physical mm approximation: use main display mm / main display logical units.
  CGDirectDisplayID main = CGMainDisplayID();
  CGSize mm = CGDisplayScreenSize(main);

  CGDisplayModeRef mainMode = CGDisplayCopyDisplayMode(main);
  double main_w_u = 0.0, main_h_u = 0.0;
  if (mainMode) {
    main_w_u = (double)CGDisplayModeGetWidth(mainMode);
    main_h_u = (double)CGDisplayModeGetHeight(mainMode);
    CFRelease(mainMode);
  }

  if (mm.width > 0 && mm.height > 0 && main_w_u > 0 && main_h_u > 0) {
    const double mmPerU_X = mm.width  / main_w_u;
    const double mmPerU_Y = mm.height / main_h_u;

    double wmm = (double)wu16 * mmPerU_X;
    double hmm = (double)hu16 * mmPerU_Y;

    if (wmm < 1.0) wmm = 1.0;
    if (hmm < 1.0) hmm = 1.0;
    if (wmm > 65535.0) wmm = 65535.0;
    if (hmm > 65535.0) hmm = 65535.0;

    if (outWmm) *outWmm = (uint16_t)(wmm + 0.5);
    if (outHmm) *outHmm = (uint16_t)(hmm + 0.5);
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


// -----------------------------
// x11_xproto_copy_window_bgra: copies host Swift surface (rootless)
// -----------------------------
#ifdef __cplusplus
extern "C" {
#endif
int x11_cpp_copy_host_surface_bgra(uint32_t xid,
                                  uint8_t* out_bytes,
                                  int32_t out_cap,
                                  int32_t* out_w,
                                  int32_t* out_h,
                                  int32_t* out_bpr);
#ifdef __cplusplus
}
#endif

int x11_xproto_copy_window_bgra(uint32_t xid,
                                uint8_t* out_bytes,
                                int32_t out_cap,
                                int32_t* out_w,
                                int32_t* out_h,
                                int32_t* out_bpr)
{
  // Swift owns all backing surfaces.  Children draw directly into the host
  // surface at their offset, so no present-time compositing is needed.
  // Just copy the host's Swift surface to the output buffer.
  return x11_cpp_copy_host_surface_bgra(xid, out_bytes, out_cap, out_w, out_h, out_bpr);
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
void x11_send_setup_success_minimal_little_endian(int fd,
                                                  uint32_t rid_base,
                                                  uint32_t rid_mask)
{
  // ---- Tunables / IDs
  const uint16_t proto_major = 11;
  const uint16_t proto_minor = 0;

  // rid_base / rid_mask are per-client; passed in.
  // (Do NOT redeclare them here.)

  const uint32_t root_xid    = 0x00000001u;
  const uint32_t root_visid  = 0x00000021u;
  const uint32_t root_cmap   = 0x00000020u;


  uint16_t screen_w_u  = 800, screen_h_u  = 600;
  uint16_t screen_w_mm = 270, screen_h_mm = 203;
  x11_get_virtual_desktop_px(&screen_w_u, &screen_h_u, &screen_w_mm, &screen_h_mm);
  fprintf( stderr, "[DISPLAY] advertised screen_w_u = %d  screen_h_u = %d\n", screen_w_u, screen_h_u);
  
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
  wr16_le(out + off + 20, screen_w_u); 
  wr16_le(out + off + 22, screen_h_u); 
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


