//
//  X11Setup.cpp
//  X11LowLevel
//
//  X11 connection setup handshake functions.
//  Moved from x11_xproto.c → C++ (extern "C" linkage preserved).
//

#include "Transport/X11Setup.hpp"
#include "Core/ScreenLayout.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cerrno>
#include <sys/socket.h>
#include <sys/types.h>

#include <CoreGraphics/CoreGraphics.h>

// For SIGPIPE avoidance on macOS
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// ---------------------------------------------------------------------------
// Wire helpers
// ---------------------------------------------------------------------------

static inline void wr16_le(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

static inline void wr32_le(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFFu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

static inline bool x11_nearly_1px(double a, double b) {
  return std::fabs(a - b) < 1.0;
}

// ---------------------------------------------------------------------------
// Socket send helper
// ---------------------------------------------------------------------------

static int x11_send_all_fd(int fd, const void* buf, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (n) {
    ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      return 0;
    }
    if (w == 0) return 0;
    p += static_cast<size_t>(w);
    n -= static_cast<size_t>(w);
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Virtual desktop dimensions (CoreGraphics query)
// ---------------------------------------------------------------------------

static void x11_get_virtual_desktop_px(uint16_t* outW, uint16_t* outH,
                                       uint16_t* outWmm, uint16_t* outHmm) {
  // Safe defaults
  if (outW) *outW = 800;
  if (outH) *outH = 600;
  if (outWmm) *outWmm = 270;
  if (outHmm) *outHmm = 203;

  CGDirectDisplayID displays[16];
  uint32_t count = 0;
  if (CGGetActiveDisplayList(static_cast<uint32_t>(sizeof(displays)/sizeof(displays[0])),
                             displays, &count) != kCGErrorSuccess || count == 0) {
    return;
  }

  // Union bounds in X11 units (points)
  double minX_u = 0, minY_u = 0, maxX_u = 0, maxY_u = 0;
  bool have = false;

  for (uint32_t i = 0; i < count; i++) {
    const CGDirectDisplayID did = displays[i];
    CGRect b = CGDisplayBounds(did);

    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(did);
    double mode_w_u = 0, mode_h_u = 0, mode_w_px = 0, mode_h_px = 0;
    double scale_x = 1.0, scale_y = 1.0;

    if (mode) {
      mode_w_u  = static_cast<double>(CGDisplayModeGetWidth(mode));
      mode_h_u  = static_cast<double>(CGDisplayModeGetHeight(mode));
      mode_w_px = static_cast<double>(CGDisplayModeGetPixelWidth(mode));
      mode_h_px = static_cast<double>(CGDisplayModeGetPixelHeight(mode));
      if (mode_w_u > 0 && mode_w_px > 0) scale_x = mode_w_px / mode_w_u;
      if (mode_h_u > 0 && mode_h_px > 0) scale_y = mode_h_px / mode_h_u;
      if (scale_x <= 0.0) scale_x = 1.0;
      if (scale_y <= 0.0) scale_y = 1.0;
    }

    const double bw = CGRectGetWidth(b);
    const double bh = CGRectGetHeight(b);

    bool bounds_is_px = false;
    bool bounds_is_u  = false;

    if (mode) {
      if (mode_w_px > 0 && x11_nearly_1px(bw, mode_w_px)) bounds_is_px = true;
      if (mode_w_u  > 0 && x11_nearly_1px(bw, mode_w_u))  bounds_is_u  = true;
      if (!bounds_is_px && mode_h_px > 0 && x11_nearly_1px(bh, mode_h_px)) bounds_is_px = true;
      if (!bounds_is_u  && mode_h_u  > 0 && x11_nearly_1px(bh, mode_h_u))  bounds_is_u  = true;
    }

    double x_u = CGRectGetMinX(b);
    double y_u = CGRectGetMinY(b);
    double w_u = bw;
    double h_u = bh;

    if (bounds_is_px && !bounds_is_u) {
      x_u /= scale_x;
      y_u /= scale_y;
      w_u /= scale_x;
      h_u /= scale_y;
    }

    const double x1_u = x_u + w_u;
    const double y1_u = y_u + h_u;

    if (!have) {
      minX_u = x_u; minY_u = y_u; maxX_u = x1_u; maxY_u = y1_u;
      have = true;
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

  const uint16_t wu16 = static_cast<uint16_t>(w_u + 0.5);
  const uint16_t hu16 = static_cast<uint16_t>(h_u + 0.5);

  if (outW) *outW = wu16;
  if (outH) *outH = hu16;

  // Physical mm approximation
  CGDirectDisplayID mainDisp = CGMainDisplayID();
  CGSize mm = CGDisplayScreenSize(mainDisp);

  CGDisplayModeRef mainMode = CGDisplayCopyDisplayMode(mainDisp);
  double main_w_u = 0.0, main_h_u = 0.0;
  if (mainMode) {
    main_w_u = static_cast<double>(CGDisplayModeGetWidth(mainMode));
    main_h_u = static_cast<double>(CGDisplayModeGetHeight(mainMode));
    CFRelease(mainMode);
  }

  if (mm.width > 0 && mm.height > 0 && main_w_u > 0 && main_h_u > 0) {
    const double mmPerU_X = mm.width  / main_w_u;
    const double mmPerU_Y = mm.height / main_h_u;

    double wmm = static_cast<double>(wu16) * mmPerU_X;
    double hmm = static_cast<double>(hu16) * mmPerU_Y;

    if (wmm < 1.0) wmm = 1.0;
    if (hmm < 1.0) hmm = 1.0;
    if (wmm > 65535.0) wmm = 65535.0;
    if (hmm > 65535.0) hmm = 65535.0;

    if (outWmm) *outWmm = static_cast<uint16_t>(wmm + 0.5);
    if (outHmm) *outHmm = static_cast<uint16_t>(hmm + 0.5);
  }
}

// ===========================================================================
// Setup handshake functions
// ===========================================================================

extern "C" void x11_send_setup_failed_le(int fd, const char* reason) {
  if (!reason) reason = "not implemented";

  uint8_t reason_len = static_cast<uint8_t>(strnlen(reason, 255));
  uint16_t major = 11, minor = 0;

  uint16_t reason_padded = static_cast<uint16_t>((reason_len + 3u) & ~3u);
  uint16_t length_words  = static_cast<uint16_t>(reason_padded / 4u);

  uint8_t hdr[8] = {0};
  hdr[0] = 0;
  hdr[1] = reason_len;
  hdr[2] = static_cast<uint8_t>(major & 0xFF);
  hdr[3] = static_cast<uint8_t>((major >> 8) & 0xFF);
  hdr[4] = static_cast<uint8_t>(minor & 0xFF);
  hdr[5] = static_cast<uint8_t>((minor >> 8) & 0xFF);
  hdr[6] = static_cast<uint8_t>(length_words & 0xFF);
  hdr[7] = static_cast<uint8_t>((length_words >> 8) & 0xFF);

  (void)x11_send_all_fd(fd, hdr, sizeof(hdr));
  if (reason_len) (void)x11_send_all_fd(fd, reason, reason_len);

  if (reason_padded > reason_len) {
    static const uint8_t zeros[4] = {0,0,0,0};
    uint16_t pad = static_cast<uint16_t>(reason_padded - reason_len);
    while (pad) {
      uint16_t chunk = (pad > 4) ? 4 : pad;
      (void)x11_send_all_fd(fd, zeros, chunk);
      pad -= chunk;
    }
  }
}

extern "C" void x11_send_setup_success_minimal_little_endian(int fd,
                                                              uint32_t rid_base,
                                                              uint32_t rid_mask) {
  const uint16_t proto_major = 11;
  const uint16_t proto_minor = 0;

  const uint32_t root_xid    = 0x00000001u;
  const uint32_t root_visid  = 0x00000021u;
  const uint32_t root_cmap   = 0x00000020u;

  // Query cached display layout (dynamic, multi-monitor aware)
  const auto layout = x11::getScreenLayout();
  const uint16_t screen_w_u  = layout.virtual_w;
  const uint16_t screen_h_u  = layout.virtual_h;
  const uint16_t screen_w_mm = layout.virtual_w_mm;
  const uint16_t screen_h_mm = layout.virtual_h_mm;
  fprintf(stderr, "[DISPLAY] advertised screen_w_u = %d  screen_h_u = %d  (%zu monitor%s)\n",
          screen_w_u, screen_h_u,
          layout.monitors.size(), layout.monitors.size() == 1 ? "" : "s");

  const char* vendor = "SwiftX11";
  const uint16_t vendor_len = static_cast<uint16_t>(std::strlen(vendor));
  const uint16_t vendor_pad = static_cast<uint16_t>((vendor_len + 3u) & ~3u);

  const uint8_t num_formats = 3;
  const uint8_t num_roots   = 1;

  const size_t fmt_bytes   = static_cast<size_t>(num_formats) * 8u;
  const size_t depth_bytes = 8u + 24u;
  const size_t root_bytes  = 40u + depth_bytes;

  const size_t setup_bytes =
      32u +
      static_cast<size_t>(vendor_pad) +
      fmt_bytes +
      root_bytes;

  const uint16_t length_words = static_cast<uint16_t>(setup_bytes / 4u);

  const size_t total_bytes = 8u + setup_bytes;
  uint8_t* out = static_cast<uint8_t*>(std::calloc(1, total_bytes));
  if (!out) return;

  size_t off = 0;

  // SetupSuccess header (8 bytes)
  out[0] = 1;
  out[1] = 0;
  out[2] = static_cast<uint8_t>(proto_major & 0xFF);
  out[3] = static_cast<uint8_t>((proto_major >> 8) & 0xFF);
  out[4] = static_cast<uint8_t>(proto_minor & 0xFF);
  out[5] = static_cast<uint8_t>((proto_minor >> 8) & 0xFF);
  out[6] = static_cast<uint8_t>(length_words & 0xFF);
  out[7] = static_cast<uint8_t>((length_words >> 8) & 0xFF);
  off = 8;

  // xConnSetup (32 bytes)
  wr32_le(out + off + 0, 1);            // release_number
  wr32_le(out + off + 4, rid_base);     // resource_id_base
  wr32_le(out + off + 8, rid_mask);     // resource_id_mask
  wr32_le(out + off + 12, 0);           // motion_buffer_size
  wr16_le(out + off + 16, vendor_len);  // nbytesVendor
  wr16_le(out + off + 18, 0xFFFF);      // max_request_size
  out[off + 20] = num_roots;
  out[off + 21] = num_formats;
  out[off + 22] = 0;  // imageByteOrder: LSBFirst
  out[off + 23] = 0;  // bitmapBitOrder: LSBFirst
  out[off + 24] = 32; // bitmapScanlineUnit
  out[off + 25] = 32; // bitmapScanlinePad
  out[off + 26] = 8;  // minKeyCode
  out[off + 27] = 255; // maxKeyCode
  off += 32;

  // vendor string (padded)
  std::memcpy(out + off, vendor, vendor_len);
  off += vendor_pad;

  // xPixmapFormat list (8 bytes each)
  // #1: depth=1, bpp=1, scanline_pad=32
  out[off + 0] = 1; out[off + 1] = 1; out[off + 2] = 32;
  off += 8;
  // #2: depth=24, bpp=32, scanline_pad=32
  out[off + 0] = 24; out[off + 1] = 32; out[off + 2] = 32;
  off += 8;
  // #3: depth=32, bpp=32, scanline_pad=32
  out[off + 0] = 32; out[off + 1] = 32; out[off + 2] = 32;
  off += 8;

  // xWindowRoot (40 bytes)
  wr32_le(out + off +  0, root_xid);
  wr32_le(out + off +  4, root_cmap);
  wr32_le(out + off +  8, 0x00FFFFFF);  // whitePixel (TrueColor: pixel IS the color)
  wr32_le(out + off + 12, 0x00000000); // blackPixel
  wr32_le(out + off + 16, 0);           // currentInputMasks
  wr16_le(out + off + 20, screen_w_u);
  wr16_le(out + off + 22, screen_h_u);
  wr16_le(out + off + 24, screen_w_mm);
  wr16_le(out + off + 26, screen_h_mm);
  wr16_le(out + off + 28, 1);           // minInstalledMaps
  wr16_le(out + off + 30, 1);           // maxInstalledMaps
  wr32_le(out + off + 32, root_visid);
  out[off + 36] = 0;  // backingStores
  out[off + 37] = 0;  // saveUnders
  out[off + 38] = 24; // rootDepth
  out[off + 39] = 1;  // nDepths
  off += 40;

  // xDepth (8 bytes): depth=24, nVisuals=1
  out[off + 0] = 24;
  out[off + 1] = 0;
  wr16_le(out + off + 2, 1);
  off += 8;

  // xVisualType (24 bytes): TrueColor visual
  wr32_le(out + off + 0, root_visid);
  wr16_le(out + off + 6, 256);
  wr32_le(out + off + 8, 0x00FF0000u);
  wr32_le(out + off + 12, 0x0000FF00u);
  wr32_le(out + off + 16, 0x000000FFu);
  out[off + 4] = 4;  // class = TrueColor
  out[off + 5] = 8;  // bitsPerRGB
  off += 24;

#ifndef NDEBUG
  if (off != total_bytes) {
    fprintf(stderr, "[SwiftX11] xproto: SetupSuccess size mismatch off=%zu total=%zu\n",
            off, total_bytes);
  }
#endif

  (void)x11_send_all_fd(fd, out, total_bytes);
  std::free(out);
}
