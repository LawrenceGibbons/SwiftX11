//
//  x11_xproto.c
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/7/26.
//

#include "x11_xproto.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "x11_requests.h" // Option B: enqueue window ops later


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
static _Atomic int g_stop = 0;
static _Atomic int g_running = 0;
static int g_lfd = -1;
static pthread_t g_thread;

// For SIGPIPE avoidance on macOS
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int x11_send_all(int fd, const void* buf, size_t n)
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

  (void)x11_send_all(fd, hdr, sizeof(hdr));
  if (reason_len) (void)x11_send_all(fd, reason, reason_len);

  if (reason_padded > reason_len) {
    static const uint8_t zeros[4] = {0,0,0,0};
    uint16_t pad = (uint16_t)(reason_padded - reason_len);
    while (pad) {
      uint16_t chunk = (pad > 4) ? 4 : pad;
      (void)x11_send_all(fd, zeros, chunk);
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
  const uint8_t num_formats = 1;
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
  *(uint32_t*)(out + off +  0) = 1;
  // resource_id_base / mask
  *(uint32_t*)(out + off +  4) = rid_base;
  *(uint32_t*)(out + off +  8) = rid_mask;
  // motion_buffer_size
  *(uint32_t*)(out + off + 12) = 0;
  // nbytesVendor
  *(uint16_t*)(out + off + 16) = vendor_len;
  // max_request_size (in 4-byte units)
  *(uint16_t*)(out + off + 18) = 0xFFFF;
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

  // xPixmapFormat (8 bytes)
  // depth=24, bpp=32, scanline_pad=32
  out[off + 0] = 24;
  out[off + 1] = 32;
  out[off + 2] = 32;
  // remaining 5 bytes pad=0
  off += 8;

  // xWindowRoot (40 bytes)
  *(uint32_t*)(out + off +  0) = root_xid;   // root
  *(uint32_t*)(out + off +  4) = root_cmap;  // defaultColormap
  *(uint32_t*)(out + off +  8) = 0;          // whitePixel
  *(uint32_t*)(out + off + 12) = 0;          // blackPixel
  *(uint32_t*)(out + off + 16) = 0;          // currentInputMasks
  *(uint16_t*)(out + off + 20) = screen_w_px;
  *(uint16_t*)(out + off + 22) = screen_h_px;
  *(uint16_t*)(out + off + 24) = screen_w_mm;
  *(uint16_t*)(out + off + 26) = screen_h_mm;
  *(uint16_t*)(out + off + 28) = 1;          // minInstalledMaps
  *(uint16_t*)(out + off + 30) = 1;          // maxInstalledMaps
  *(uint32_t*)(out + off + 32) = root_visid; // rootVisualID
  out[off + 36] = 0;                         // backingStores
  out[off + 37] = 0;                         // saveUnders
  out[off + 38] = 24;                        // rootDepth
  out[off + 39] = 1;                         // nDepths
  off += 40;

  // xDepth (8 bytes): depth=24, nVisuals=1
  out[off + 0] = 24;
  out[off + 1] = 0;
  *(uint16_t*)(out + off + 2) = 1;           // nVisuals
  // pad 4 bytes
  off += 8;

  // xVisualType (24 bytes): TrueColor visual
  *(uint32_t*)(out + off +  0) = root_visid; // visualid
  out[off +  4] = 4;                         // class = TrueColor
  out[off +  5] = 8;                         // bitsPerRGB
  *(uint16_t*)(out + off +  6) = 256;        // colormapEntries
  *(uint32_t*)(out + off +  8) = 0x00FF0000u; // redMask
  *(uint32_t*)(out + off + 12) = 0x0000FF00u; // greenMask
  *(uint32_t*)(out + off + 16) = 0x000000FFu; // blueMask
  // pad 4 bytes
  off += 24;

  // Defensive: ensure we filled exactly what we computed.
  if (off != total_bytes) {
#ifndef NDEBUG
    fprintf(stderr, "[SwiftX11] xproto: SetupSuccess size mismatch off=%zu total=%zu\n", off, total_bytes);
#endif
    // still try sending what we built
  }

  (void)x11_send_all(fd, out, total_bytes);
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
static atom_entry_t* g_atoms = NULL;
static size_t g_atoms_n = 0;
static size_t g_atoms_cap = 0;
static uint32_t g_next_atom = 1; // 0 is None

static uint32_t atoms_intern(const char* name, size_t len, bool only_if_exists)
{
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
// Request handlers (the “6”)
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
  (void)x11_send_all(fd, rep, sizeof(rep));
}

static void handle_ListExtensions(int fd, uint16_t seq)
{
  // Reply: nExtensions=0, length=0
  uint8_t rep[32];
  x11_reply32_le(rep, seq, 0);
  rep[1] = 0; // nExtensions
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

  uint8_t rep[32];
  x11_reply32_le(rep, seq, 0);
  rep[8]  = (uint8_t)(atom & 0xFF);
  rep[9]  = (uint8_t)((atom >> 8) & 0xFF);
  rep[10] = (uint8_t)((atom >> 16) & 0xFF);
  rep[11] = (uint8_t)((atom >> 24) & 0xFF);
  (void)x11_send_all(fd, rep, sizeof(rep));
}

static void handle_GetAtomName(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
  // Request body after 4-byte header:
  //   CARD32 atom
  if (remain < 4) return;
  uint32_t atom = (uint32_t)((uint32_t)payload[0]
                          | ((uint32_t)payload[1] << 8)
                          | ((uint32_t)payload[2] << 16)
                          | ((uint32_t)payload[3] << 24));

  size_t name_len = 0;
  const char* name = atoms_name(atom, &name_len);
  if (!name) { name = ""; name_len = 0; }

  const uint16_t n = (name_len > 65535u) ? 65535u : (uint16_t)name_len;
  const uint16_t pad = (uint16_t)((n + 3u) & ~3u);
  const uint32_t extra_words = (uint32_t)(pad / 4u);

  uint8_t rep[32];
  x11_reply32_le(rep, seq, extra_words);
  // name length at bytes 8..9
  rep[8] = (uint8_t)(n & 0xFF);
  rep[9] = (uint8_t)((n >> 8) & 0xFF);

  (void)x11_send_all(fd, rep, sizeof(rep));
  if (n) (void)x11_send_all(fd, name, n);
  if (pad > n) {
    static const uint8_t zeros[4] = {0,0,0,0};
    uint16_t p = (uint16_t)(pad - n);
    while (p) {
      uint16_t chunk = (p > 4) ? 4 : p;
      (void)x11_send_all(fd, zeros, chunk);
      p -= chunk;
    }
  }
}

static void handle_GetProperty(int fd, uint16_t seq)
{
  // Return “no such property”: format=0, type=None, bytesAfter=0, nItems=0, length=0
  uint8_t rep[32];
  x11_reply32_le(rep, seq, 0);
  rep[1] = 0; // format
  // type(None)=0 at bytes 8..11 already 0
  // bytesAfter 12..15 already 0
  // nItems 16..19 already 0
  (void)x11_send_all(fd, rep, sizeof(rep));
}

static void handle_CreateGC(void)
{
  // For now: accept silently, don’t error.
  // (Later: track GCs per connection so xterm doesn’t explode.)
}

// ----------------------------------------------------------------------------
// Request pump + dispatcher
// ----------------------------------------------------------------------------
static void drain_requests(int cfd)
{
  // Small recv timeout so we can notice stop without blocking forever.
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100 * 1000;
  (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv));

  uint16_t seq = 0;

  for (;;) {
    if (atomic_load_explicit(&g_stop, memory_order_relaxed)) break;

    uint8_t hdr[4];
    int hr = x11_recv_all(cfd, hdr, sizeof(hdr));
    if (hr == 0) break;
    if (hr == -2) continue;
    if (hr < 0) break;

    const uint8_t major = hdr[0];
    const uint8_t minor = hdr[1];
    const uint16_t len_words = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));
    if (len_words == 0) break;

    const size_t total = (size_t)len_words * 4u;
    if (total < 4u) break;
    const size_t remain = total - 4u;

    seq++;

#ifndef NDEBUG
    fprintf(stderr, "[SwiftX11] xproto: req major=%u minor=%u len_words=%u (%zu bytes)\n",
            (unsigned)major, (unsigned)minor, (unsigned)len_words, total);
#endif

    uint8_t stack_buf[4096];
    uint8_t* payload = stack_buf;
    uint8_t* heap_buf = NULL;

    if (remain > sizeof(stack_buf)) {
      heap_buf = (uint8_t*)malloc(remain);
      if (!heap_buf) break;
      payload = heap_buf;
    }

    // read request body
    size_t off = 0;
    while (off < remain) {
      int rr = x11_recv_all(cfd, payload + off, remain - off);
      if (rr == 1) { off = remain; break; }
      if (rr == 0) { off = remain; break; }
      if (rr == -2) {
        if (atomic_load_explicit(&g_stop, memory_order_relaxed)) { off = remain; break; }
        continue;
      }
      off = remain;
      break;
    }

    // Dispatch
    switch (major) {
      case 98: // QueryExtension
        handle_QueryExtension(cfd, seq);
        break;

      case 99: // ListExtensions
        handle_ListExtensions(cfd, seq);
        break;

      case 16: // InternAtom
        handle_InternAtom(cfd, seq, payload, remain, (minor != 0));
        break;

      case 17: // GetAtomName
        handle_GetAtomName(cfd, seq, payload, remain);
        break;

      case 20: // GetProperty
        handle_GetProperty(cfd, seq);
        break;

      case 55: // CreateGC (no reply)
        handle_CreateGC();
        break;

      default:
        // For now: ignore unknown requests.
        // A “real” server might send Error; we’ll stay permissive to keep clients running.
        break;
    }

    if (heap_buf) free(heap_buf);
  }
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

#ifndef NDEBUG
    fprintf(stderr, "[SwiftX11] xproto: client connected (byte_order=l), replying SetupSuccess(minimal)\n");
#endif

    x11_send_setup_success_minimal_little_endian(cfd);

    // Now drain requests
    drain_requests(cfd);

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

#ifndef NDEBUG
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

#ifndef NDEBUG
  fprintf(stderr, "[SwiftX11] xproto: listener stopped\n");
#endif
}
