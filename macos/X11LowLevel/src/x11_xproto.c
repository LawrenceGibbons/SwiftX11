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

#include "x11_requests.h"

// Forward decls (used by enqueue helpers near top of file)
static const char* atoms_name(uint32_t atom, size_t* out_len);

// -----------------------------------------------------------------------------
// enqueue requests to be consumed by x11_shim (via x11_requests_* queue)
// -----------------------------------------------------------------------------
static void enqueue_create_window(uint32_t xid, uint32_t parent, int16_t x, int16_t y,
                                  uint16_t w, uint16_t h, uint32_t event_mask)
{
  (void)parent; (void)x; (void)y; (void)event_mask;
  char title[64];
  snprintf(title, sizeof(title), "xid=0x%08X", (unsigned)xid);
  (void)x11_requests_push_create(xid, title, (int32_t)w, (int32_t)h);
}

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
  uint8_t  mapped;      // 0/1
  uint32_t event_mask;  // from ChangeWindowAttributes / CreateWindow
  int owner_fd;   // client socket that created this window
} x11_win_t;


static x11_win_t g_wins[256];
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

static x11_win_t* win_find(uint32_t xid)
{
  for (size_t i = 0; i < g_wins_n; i++) if (g_wins[i].xid == xid) return &g_wins[i];
  return NULL;
}

static x11_win_t* win_add(uint32_t xid)
{
  if (g_wins_n >= (sizeof(g_wins)/sizeof(g_wins[0]))) return NULL;
  x11_win_t* w = &g_wins[g_wins_n++];
  memset(w, 0, sizeof(*w));
  w->xid = xid;
  return w;
}

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


static void send_Expose(int fd, uint16_t seq, uint32_t wid, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h, uint16_t count){
  // Expose event: type=12
  uint8_t ev[32];
  memset(ev, 0, sizeof(ev));
  ev[0] = 12; // Expose
  // ev[1] unused
  wr16_le(ev + 2, seq);
  wr32_le(ev + 4, wid);
  wr16_le(ev + 8, x);
  wr16_le(ev + 10, y);
  wr16_le(ev + 12, w);
  wr16_le(ev + 14, h);
  wr16_le(ev + 16, count);
  (void)x11_send_all(fd, ev, sizeof(ev));
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

  // xPixmapFormat (8 bytes)
  // depth=24, bpp=32, scanline_pad=32
  out[off + 0] = 24;
  out[off + 1] = 32;
  out[off + 2] = 32;
  // remaining 5 bytes pad=0
  off += 8;

  // xWindowRoot (40 bytes)
  wr32_le(out + off + 0, root_xid);     // root
  wr32_le(out + off + 4, root_cmap);    // defaultColormap
  wr32_le(out + off + 8, 0);            // whitePixel
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

  uint8_t rep[32];
  x11_reply32_le(rep, seq, 0);
  rep[8]  = (uint8_t)(atom & 0xFF);
  rep[9]  = (uint8_t)((atom >> 8) & 0xFF);
  rep[10] = (uint8_t)((atom >> 16) & 0xFF);
  rep[11] = (uint8_t)((atom >> 24) & 0xFF);
  
#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] xproto: REPLY op=handle_InternAtom seq=%u bytes=%zu length_words=%u\n",
          (unsigned)seq,
          (size_t)sizeof(rep),
          (unsigned)rd32(rep + 4));
  dbg_check_reply_total("InternAtom", seq, 32, rep);
#endif
  
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

#ifndef NDEBUG
  dbg_check_reply_header32("GetAtomName", seq, rep);
#endif
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

  // Return black for every queried pixel (we're not implementing colormaps yet).
  // xrgb: red=0, green=0, blue=0, pad=0
  static const uint8_t xrgb0[8] = {0,0,0,0,0,0,0,0};
  for (uint16_t i = 0; i < ncolors; i++) {
    (void)i; // pixel list is ignored for now
    (void)x11_send_all(fd, xrgb0, sizeof(xrgb0));
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

// ChangeWindowAttributes (major = 2)
static void handle_ChangeWindowAttributes(const uint8_t* payload, size_t remain)
{
  // Body (after 4-byte header):
  //   CARD32 window
  //   CARD32 valueMask
  //   LISTofCARD32 valueList
  if (remain < 8) return;

  const uint32_t wid   = rd32(payload + 0);
  const uint32_t vmask = rd32(payload + 4);

  x11_win_t* w = win_find(wid);
  if (!w) return;

  const uint8_t* vp = payload + 8;
  size_t vrem = remain - 8;
  apply_value_list_updates_for_window(w, vmask, vp, vrem);
}

// UnmapWindow (major = 10)
static void handle_UnmapWindow(int fd, const uint8_t* payload, size_t remain)
{
  (void)fd;
  if (remain < 4) return;
  uint32_t wid = rd32(payload + 0);
  x11_win_t* w = win_find(wid);
  if (!w) return;
  w->mapped = 0;
  // enqueue to shim
  enqueue_unmap_window(wid);
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
          changed = true;
        }
      }

      if (ch->mapped) {
#if !defined(NDEBUG) && SWIFTX11_TRACE
        mapped_count++;
#endif
        // enqueue to shim
        enqueue_map_window(ch->xid);
        
        // Only send Expose if the client selected ExposureMask on THAT window.
        if (ch->event_mask & (1u << 15)) { // ExposureMask
          send_Expose(fd, seq, ch->xid, 0, 0, ch->w, ch->h, 0);
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
      // remove by swap-with-last
      g_wins[i] = g_wins[g_wins_n - 1];
      g_wins_n--;
      break;
    }
  }
  // enqueue to shim
  enqueue_destroy_window(wid);
}

// ConfigureWindow (major = 12)
static void handle_ConfigureWindow(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
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

  // enqueue to shim
  enqueue_configure_window(wid, w->x, w->y, w->w, w->h);
  
  // If mapped and client selected for Exposure, send another Expose.
  if (w->mapped && (w->event_mask & (1u << 15))) { // ExposureMask = (1<<15)
    send_Expose(fd, seq, wid, 0, 0, w->w, w->h, 0);
  }
}


static void handle_CreateGC(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
  (void)fd; (void)seq; (void)payload; (void)remain;
  // no reply for CreateGC
}

// create window (major = 1)
static void handle_CreateWindow(uint8_t depth, const uint8_t* payload, size_t remain)
{
  // CreateWindow request body (after 4-byte header) begins with:
  // 4: wid
  // 4: parent
  // 2: x
  // 2: y
  // 2: width
  // 2: height
  // 2: borderWidth
  // 2: class
  // 4: visual
  // 4: valueMask
  // then value-list (optional)
  if (remain < 28) return;

  uint32_t wid    = rd32(payload + 0);
  uint32_t parent = rd32(payload + 4);
  int16_t  x      = (int16_t)rd16(payload + 8);
  int16_t  y      = (int16_t)rd16(payload + 10);
  uint16_t wpx    = rd16(payload + 12);
  uint16_t hpx    = rd16(payload + 14);
  uint32_t vmask  = rd32(payload + 24);

  // Create or overwrite (idempotent-ish for now)
  x11_win_t* w = win_find(wid);
  if (!w) w = win_add(wid);
  if (!w) return;

  w->parent = parent;
  w->x = x; w->y = y;
  w->w = (wpx ? wpx : 1);
  w->h = (hpx ? hpx : 1);
  w->mapped = 0;
  w->event_mask = 0;
  w->owner_fd = g_current_client_fd;
  
  // If valueMask includes CWEventMask (bit 11) we should read it from value-list.
  // valueMask bits are defined by X11; CWEventMask = (1<<11).
  // value-list starts immediately after the fixed portion (28 bytes).
  if (vmask & (1u << 11)) {
    // value-list is 32-bit items in the order of bits set.
    // For Phase A: we only care about CWEventMask, so we can scan in-order.
    const uint8_t* vp = payload + 28;
    size_t vrem = remain - 28;
    uint32_t cur_mask = 0;

    for (uint32_t bit = 0; bit < 32; bit++) {
      if (!(vmask & (1u << bit))) continue;
      if (vrem < 4) break;
      uint32_t val = rd32(vp);
      if (bit == 11) cur_mask = val; // CWEventMask
      vp += 4; vrem -= 4;
    }
    w->event_mask = cur_mask;
  }

  // enqueue to shim
  enqueue_create_window(wid, parent, w->x, w->y, w->w, w->h, w->event_mask);

#if !defined(NDEBUG) && SWIFTX11_TRACE
  fprintf(stderr,
          "[SwiftX11] xproto: CreateWindow wid=0x%08X parent=0x%08X vmask=0x%08X event_mask=0x%08X\n",
          (unsigned)wid, (unsigned)parent, (unsigned)vmask, (unsigned)w->event_mask);
#endif

  (void)depth;
}


// map window (major = 8)
static void handle_MapWindow(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
  if (remain < 4) return;
  uint32_t wid = rd32(payload + 0);
  x11_win_t* w = win_find(wid);
  if (!w) return;

#if !defined(NDEBUG) && SWIFTX11_TRACE
  fprintf(stderr,
          "[SwiftX11] xproto: MapWindow wid=0x%08X seq=%u event_mask=0x%08X\n",
          (unsigned)wid, (unsigned)seq, (unsigned)w->event_mask);
#endif

  w->mapped = 1;
  // enqueue to shim
  enqueue_map_window(wid);
  
  if (w->event_mask & (1u << 15)) {
    send_Expose(fd, seq, wid, 0, 0, w->w, w->h, 0);
  }
}


// get geometry (major = 14)
static void handle_GetGeometry(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
  if (remain < 4) return;
  uint32_t drawable = rd32(payload + 0);

  // Root geometry or window geometry
  uint32_t root = X11_ROOT_XID;
  int16_t x = 0, y = 0;
  uint16_t wpx = 800, hpx = 600;
  uint16_t border = 0;

  x11_win_t* w = win_find(drawable);
  if (w) {
    root = X11_ROOT_XID;
    x = w->x; y = w->y;
    wpx = w->w; hpx = w->h;
  }

  uint8_t rep[32];
  x11_reply32_le(rep, seq, 0);
  wr32_le(rep + 8, root);
  wr16_le(rep + 12, (uint16_t)x);
  wr16_le(rep + 14, (uint16_t)y);
  wr16_le(rep + 16, wpx);
  wr16_le(rep + 18, hpx);
  wr16_le(rep + 20, border);
  wr16_le(rep + 22, 24);
#ifndef NDEBUG
  dbg_check_reply_total("GetGeometry", seq, 32, rep);
#endif
  (void)x11_send_all(fd, rep, sizeof(rep));
}


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


// GetInputFocus (major = 43)
static void handle_GetInputFocus(int fd, uint16_t seq)
{
  // Reply format (32 bytes):
  //  byte 0: 1 (Reply)
  //  byte 1: revert-to (we'll use 0 = None)
  //  bytes 2-3: sequence
  //  bytes 4-7: length (0)
  //  bytes 8-11: focus window (XID) (we'll use root for now)
  uint8_t rep[32];
  x11_reply32_le(rep, seq, 0);
  rep[1] = 0; // revertTo = None
  wr32_le(rep + 8, X11_ROOT_XID);           // focus = root
  
#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] xproto: REPLY op=handle_GetInputFocus seq=%u bytes=%zu length_words=%u\n",
          (unsigned)seq,
          (size_t)sizeof(rep),
          (unsigned)rd32(rep + 4));
  dbg_check_reply_total("GetInputFocus", seq, sizeof(rep), rep);

#endif
  (void)x11_send_all(fd, rep, sizeof(rep));
}

// QueryTree (major = 15)
static void handle_QueryTree(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
  if (remain < 4) return;

  const uint32_t wid = rd32(payload + 0);

  uint32_t root = X11_ROOT_XID;
  uint32_t parent = 0;

  // Collect children for any window (not just root)
  uint32_t children[256];
  uint16_t nchildren = 0;

  {
    x11_win_t* w = win_find(wid);
    if (wid == X11_ROOT_XID) {
      parent = 0;
    } else if (w) {
      parent = w->parent;
    } else {
      parent = 0;
    }

    // children = all windows whose parent is `wid`
    for (size_t i = 0; i < g_wins_n && nchildren < 256; i++) {
      if (g_wins[i].parent == wid) {
        children[nchildren++] = g_wins[i].xid;
      }
    }
  }

  // Reply length is extra data beyond the 32-byte reply header, in 4-byte units.
  const uint32_t extra_words = (uint32_t)nchildren; // each child is CARD32

  uint8_t rep[32];
  x11_reply32_le(rep, seq, extra_words);

  // root at bytes 8..11
  wr32_le(rep + 8, root);
  // parent at bytes 12..15
  wr32_le(rep + 12, parent);
  // nchildren at bytes 16..17 (CARD16)
  wr16_le(rep + 16, nchildren);
  // remaining pad bytes are already 0

#ifndef NDEBUG
  dbg_check_reply_header32("QueryTree", seq, rep);
#endif
  (void)x11_send_all(fd, rep, sizeof(rep));

  // Follow with children list (CARD32[]), little-endian
  if (nchildren) {
    uint8_t out[256 * 4];
    for (uint16_t i = 0; i < nchildren; i++) {
      wr32_le(out + (size_t)i * 4u, children[i]);
    }
    (void)x11_send_all(fd, out, (size_t)nchildren * 4u);
  }
}


static void handle_GetProperty(int fd, uint16_t seq, uint8_t delete_flag,
                               const uint8_t* payload, size_t remain)
{
  // GetProperty request body after 4-byte header (20 bytes):
  //   CARD32 window
  //   CARD32 property
  //   CARD32 type
  //   CARD32 longOffset   (in 4-byte units)
  //   CARD32 longLength   (in 4-byte units)
  if (remain < 20) return;

  const uint32_t wid        = rd32(payload + 0);
  const uint32_t prop_atom  = rd32(payload + 4);
  const uint32_t req_type   = rd32(payload + 8);
  const uint32_t long_off   = rd32(payload + 12);
  const uint32_t long_len   = rd32(payload + 16);

  x11_prop_t* p = prop_find(wid, prop_atom);

  // Default reply: “no such property” (format=0, type=None, everything else 0).
  uint8_t rep[32];
  x11_reply32_le(rep, seq, 0);
  rep[1] = 0; // format

  if (!p || p->format == 0 || p->nbytes == 0 || !p->data) {
#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] xproto: REPLY op=handle_GetProperty (no such property) seq=%u bytes=%zu length_words=%u\n",
          (unsigned)seq,
          (size_t)sizeof(rep),
          (unsigned)rd32(rep + 4));
    dbg_check_reply_total("GetProperty (1st)", seq, sizeof(rep), rep);
#endif
    (void)x11_send_all(fd, rep, sizeof(rep));
    return;
  }

  // If client requested a specific type (nonzero) and it doesn't match, return empty.
  if (req_type != 0 && p->type != req_type) {
  #ifndef NDEBUG
    fprintf(stderr,
            "[SwiftX11] xproto: REPLY op=handle_GetProperty (specific type doesn't match) seq=%u bytes=%zu length_words=%u\n",
            (unsigned)seq,
            (size_t)sizeof(rep),
            (unsigned)rd32(rep + 4));
    dbg_check_reply_header32("GetProperty", seq, rep);
#endif
    (void)x11_send_all(fd, rep, sizeof(rep));
    return;
  }

  // Validate format.
  const uint8_t fmt = p->format;
  uint32_t unit_bytes = 0;
  if (fmt == 8)  unit_bytes = 1;
  if (fmt == 16) unit_bytes = 2;
  if (fmt == 32) unit_bytes = 4;
  if (unit_bytes == 0) {
#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] xproto: REPLY op=handle_GetProperty (unit_bytes=0) seq=%u bytes=%zu length_words=%u\n",
          (unsigned)seq,
          (size_t)sizeof(rep),
          (unsigned)rd32(rep + 4));
    dbg_check_reply_header32("GetProperty", seq, rep);
#endif
    (void)x11_send_all(fd, rep, sizeof(rep));
    return;
  }

  const uint32_t total_bytes = p->nbytes;

  // Offset/length are expressed in 4-byte units regardless of format.
  // Use 64-bit math to avoid overflow.
  const uint64_t byte_off64  = (uint64_t)long_off * 4ull;
  const uint64_t max_bytes64 = (uint64_t)long_len * 4ull;

  uint32_t send_off = (byte_off64 >= (uint64_t)total_bytes) ? total_bytes : (uint32_t)byte_off64;
  uint32_t remain_bytes = total_bytes - send_off;

  // longLength==0 means “return zero bytes” (still must report bytesAfter correctly).
  uint32_t send_bytes = remain_bytes;
  if (max_bytes64 < (uint64_t)send_bytes) send_bytes = (uint32_t)max_bytes64;

  // For 16/32 formats, be defensive: only return whole items.
  if (unit_bytes > 1) {
    send_bytes -= (send_bytes % unit_bytes);
  }

  // bytesAfter is the number of bytes remaining in the property after the portion returned.
  const uint32_t bytes_after = remain_bytes - send_bytes;

  // nItems is in units of format.
  uint32_t nitems = 0;
  if (send_bytes && unit_bytes) nitems = send_bytes / unit_bytes;

  // Pad reply data to 4 bytes; reply length counts 4-byte units after the 32-byte header.
  const uint32_t pad_bytes   = (uint32_t)((send_bytes + 3u) & ~3u);
  const uint32_t extra_words = pad_bytes / 4u;

  x11_reply32_le(rep, seq, extra_words);
  rep[1] = fmt;                 // format
  wr32_le(rep + 8, p->type);
  wr32_le(rep + 12, bytes_after);
  wr32_le(rep + 16, nitems);
  
#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] xproto: REPLY op=handle_GetProperty (after pad reply to 4 bytes) seq=%u bytes=%zu length_words=%u\n",
          (unsigned)seq,
          (size_t)sizeof(rep),
          (unsigned)rd32(rep + 4));
  dbg_check_reply_header32("GetProperty", seq, rep);
#endif
  (void)x11_send_all(fd, rep, sizeof(rep));

#ifndef NDEBUG
  fprintf(stderr,
          "[SwiftX11] xproto: REPLY op=handle_GetProperty (in if send_bytes) seq=%u bytes=%zu length_words=%u\n",
          (unsigned)seq,
          (size_t)sizeof(rep),
          (unsigned)rd32(rep + 4));
#endif
  if (send_bytes) {
    (void)x11_send_all(fd, p->data + send_off, send_bytes);
  }

  if (pad_bytes > send_bytes) {
    static const uint8_t zeros[4] = {0,0,0,0};
    uint32_t pad = pad_bytes - send_bytes;
    while (pad) {
      uint32_t chunk = (pad > 4u) ? 4u : pad;
      (void)x11_send_all(fd, zeros, chunk);
      pad -= chunk;
    }
  }

  // Delete property if requested and we returned the entire property starting at offset 0.
  if (delete_flag && long_off == 0 && send_bytes == total_bytes) {
    prop_delete(wid, prop_atom);
  }
}

// ChangeProperty (major = 18) -- no reply
static void handle_ChangeProperty(uint8_t mode, const uint8_t* payload, size_t remain)
{
  // Body after 4-byte header:
  //   CARD32 window
  //   CARD32 property
  //   CARD32 type
  //   CARD8  format (8/16/32)
  //   3      pad
  //   CARD32 nUnits
  //   LISTofBYTE data (padded to 4)
  if (remain < 20) return;

  const uint32_t wid    = rd32(payload + 0);
  const uint32_t atom   = rd32(payload + 4);
  const uint32_t type   = rd32(payload + 8);
  const uint8_t  fmt    = payload[12];
  const uint32_t nunits = rd32(payload + 16);

  uint32_t unit_bytes = 0;
  if (fmt == 8)  unit_bytes = 1;
  if (fmt == 16) unit_bytes = 2;
  if (fmt == 32) unit_bytes = 4;
  if (unit_bytes == 0) return;

  // Compute byte count safely.
  const uint64_t data_bytes_64 = (uint64_t)nunits * (uint64_t)unit_bytes;
  if (data_bytes_64 > 0xFFFFFFFFu) return;
  const uint32_t data_bytes = (uint32_t)data_bytes_64;

  // The request may include trailing padding to 4 bytes; remain includes that padding.
  if (remain < 20u + (size_t)data_bytes) return;

  const uint8_t* data = payload + 20;
  
  // enqueue to shim (best-effort: only for title-related properties)
  enqueue_maybe_set_title(wid, atom, type, fmt, data, data_bytes);  
  // mode (from request header byte1):
  //   0 = Replace, 1 = Prepend, 2 = Append
  if (mode == 1) {
    prop_prepend_append(wid, atom, type, fmt, data, data_bytes, 0 /*append*/);
  } else if (mode == 2) {
    prop_prepend_append(wid, atom, type, fmt, data, data_bytes, 1 /*append*/);
  } else {
    prop_set_bytes(wid, atom, type, fmt, data, data_bytes);
  }
}

// PolyFillRectangle (major = 70) -- no reply
static void handle_PolyFillRectangle_noop(const uint8_t* payload, size_t remain)
{
  // Body after 4-byte header:
  //   CARD32 drawable
  //   CARD32 gc
  //   LISTofxRectangle rectangles (each 8 bytes)
  if (remain < 8) return;

#if !defined(NDEBUG) && SWIFTX11_TRACE_NOOP_DRAW
  const uint32_t drawable = rd32(payload + 0);
  const uint32_t gc       = rd32(payload + 4);
  const size_t list_bytes = remain - 8u;
  const size_t nrects     = list_bytes / 8u;
  fprintf(stderr,
          "[SwiftX11] xproto: PolyFillRectangle(noop) drawable=0x%08X gc=0x%08X nrects=%zu remain=%zu\n",
          (unsigned)drawable, (unsigned)gc, nrects, remain);
#endif

  // No-op: ignore draw request for bring-up.
}

// PolyFillArc (major = 71) -- no reply
static void handle_PolyFillArc_noop(const uint8_t* payload, size_t remain)
{
  // Body after 4-byte header:
  //   CARD32 drawable
  //   CARD32 gc
  //   LISTofxArc arcs (each 12 bytes)
  if (remain < 8) return;

#if !defined(NDEBUG) && SWIFTX11_TRACE_NOOP_DRAW
  const uint32_t drawable = rd32(payload + 0);
  const uint32_t gc       = rd32(payload + 4);
  const size_t list_bytes = remain - 8u;
  const size_t narcs      = list_bytes / 12u;
  fprintf(stderr,
          "[SwiftX11] xproto: PolyFillArc(noop) drawable=0x%08X gc=0x%08X narcs=%zu remain=%zu\n",
          (unsigned)drawable, (unsigned)gc, narcs, remain);
#endif

  // No-op: ignore draw request for bring-up.
}


// QueryPointer (major = 38 / 0x26)
static void handle_QueryPointer(int fd, uint16_t seq, const uint8_t* payload, size_t remain)
{
  if (remain < 4) return;
  const uint32_t qwin = rd32(payload + 0);

  x11_win_t* w = win_find(qwin);

  // Fake pointer: slowly moves in a small box (so xeyes has something to do).
  static int16_t fake_rx = 0, fake_ry = 0;
  fake_rx = (int16_t)((fake_rx + 1) % 200);
  fake_ry = (int16_t)((fake_ry + 1) % 120);

  uint32_t child = 0;
  int16_t winx = 0, winy = 0;

  if (w && w->mapped) {
    child = qwin;

    // Translate root -> window coords
    int32_t tx = (int32_t)fake_rx - (int32_t)w->x;
    int32_t ty = (int32_t)fake_ry - (int32_t)w->y;

    // Clamp to window bounds (optional but nice)
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if (tx > (int32_t)w->w) tx = (int32_t)w->w;
    if (ty > (int32_t)w->h) ty = (int32_t)w->h;

    winx = (int16_t)tx;
    winy = (int16_t)ty;
  }

  uint8_t rep[32];
  x11_reply32_le(rep, seq, 0);

  rep[1] = 1; // sameScreen = true

  wr32_le(rep + 8,  X11_ROOT_XID); // root
  wr32_le(rep + 12, child);        // child

  wr16_le(rep + 16, (uint16_t)fake_rx); // rootX
  wr16_le(rep + 18, (uint16_t)fake_ry); // rootY
  wr16_le(rep + 20, (uint16_t)winx);    // winX
  wr16_le(rep + 22, (uint16_t)winy);    // winY

  wr16_le(rep + 24, 0); // mask

#ifndef NDEBUG
  dbg_check_reply_total("QueryPointer", seq, sizeof(rep), rep);
#endif
  (void)x11_send_all(fd, rep, sizeof(rep));
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
  // Body after 4-byte header:
  //   CARD32 window
  //   INT16  x
  //   INT16  y
  //   CARD16 width
  //   CARD16 height
  // exposures is request byte1 (minor)
  if (remain < 12) return;

  const uint32_t wid = rd32(payload + 0);
  const int16_t  x   = (int16_t)rd16(payload + 4);
  const int16_t  y   = (int16_t)rd16(payload + 6);
  const uint16_t wpx = rd16(payload + 8);
  const uint16_t hpx = rd16(payload + 10);

  x11_win_t* w = win_find(wid);
  if (!w) return;

  // If exposures==false, request is a no-op for our bring-up.
  if (!exposures) return;

  // Only send Expose if window is mapped and client selected ExposureMask.
  if (!w->mapped) return;
  if (!(w->event_mask & (1u << 15))) return; // ExposureMask

  // X11 semantics: if width/height are 0, clear from (x,y) to bottom-right of window.
  // We approximate with full window if either is 0.
  uint16_t ex_w = (wpx == 0) ? w->w : wpx;
  uint16_t ex_h = (hpx == 0) ? w->h : hpx;

#if !defined(NDEBUG) && SWIFTX11_TRACE
  fprintf(stderr,
          "[SwiftX11] xproto: ClearArea exposures=1 wid=0x%08X seq=%u x=%d y=%d w=%u h=%u -> Expose %ux%u\n",
          (unsigned)wid, (unsigned)seq, (int)x, (int)y, (unsigned)wpx, (unsigned)hpx,
          (unsigned)ex_w, (unsigned)ex_h);
#endif

  send_Expose(fd, seq, wid, (uint16_t)x, (uint16_t)y, ex_w, ex_h, 0);
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
  g_current_client_fd = cfd;

  bool should_cleanup = true;

  for (;;) {
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

    #if !defined(NDEBUG) && SWIFTX11_TRACE_DUMP_70_71
    if (major == 70 || major == 71) {
      dbg_dump_req("OP70/71", major, minor, len_words, payload, remain);
    }
    #endif
    // Dispatch
    switch (major) {
      case 1: // CreateWindow
        handle_CreateWindow(minor /*depth*/, payload, remain);
        break;

      case 2: // ChangeWindowAttributes
        handle_ChangeWindowAttributes(payload, remain);
        break;

      case 3: // GetWindowAttributes
        handle_GetWindowAttributes(cfd, seq, payload, remain);
        break;

      case 4: // DestroyWindow
        handle_DestroyWindow(cfd, payload, remain);
        break;

      case 8: // MapWindow
        handle_MapWindow(cfd, seq, payload, remain);
        break;

      case 9: // MapSubwindows
        handle_MapSubwindows(cfd, seq, payload, remain);
        break;

      case 10: // UnmapWindow
        handle_UnmapWindow(cfd, payload, remain);
        break;

      case 12: // ConfigureWindow
        handle_ConfigureWindow(cfd, seq, payload, remain);
        break;

      case 14: // GetGeometry
        handle_GetGeometry(cfd, seq, payload, remain);
        break;

      case 15: // QueryTree
        handle_QueryTree(cfd, seq, payload, remain);
        break;

      case 16: // InternAtom
        handle_InternAtom(cfd, seq, payload, remain, (minor != 0));
        break;

      case 17: // GetAtomName
        handle_GetAtomName(cfd, seq, payload, remain);
        break;

      case 18: // ChangeProperty (no reply)
        handle_ChangeProperty(minor /*mode*/, payload, remain);
        break;
        
      case 20: // GetProperty
        handle_GetProperty(cfd, seq, minor /*delete*/, payload, remain);
        break;
        
      case 38: // QueryPointer
        handle_QueryPointer(cfd, seq, payload, remain);
        break;

      case 43: // GetInputFocus
        handle_GetInputFocus(cfd, seq);
        break;

      case 55: // CreateGC (no reply)
        handle_CreateGC(cfd, seq, payload, remain );
        break;

      case 61: // ClearArea (no reply; may generate Expose)
        handle_ClearArea(cfd, seq, minor /*exposures*/, payload, remain);
        break;
    
      case 70: // PolyFillRectangle (no reply)
        handle_PolyFillRectangle_noop(payload, remain);
        break;

      case 71: // PolyFillArc (no reply)
        handle_PolyFillArc_noop(payload, remain);
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
        // For now: ignore unknown requests.
        // A “real” server might send Error; we’ll stay permissive to keep clients running.
        break;
    }

    // Always free heap_buf if used
    if (heap_buf) {
      fprintf(stderr, "[SwiftX11] freeing heap_buf for major=%u\n", (unsigned)major);
      free(heap_buf);
      heap_buf = NULL;
    }
  }
  
  g_current_client_fd = -1;
  
  if (should_cleanup) {
    // Client disconnected: destroy all windows owned by this client
    for (size_t i = 0; i < g_wins_n; ) {
      if (g_wins[i].owner_fd == cfd) {
        uint32_t wid = g_wins[i].xid;
        
        // delete all properties on this window
        prop_delete_all_for_window(wid);
        
        // remove window (swap-with-last)
        g_wins[i] = g_wins[g_wins_n - 1];
        g_wins_n--;
        
        // notify shim
        enqueue_destroy_window(wid);
        continue; // re-check swapped entry
      }
      i++;
    }
  }
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

