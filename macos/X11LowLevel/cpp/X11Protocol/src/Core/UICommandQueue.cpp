//
//  UICommandQueue.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/6/26.
//
// Server -> Swift UI command queue (C++ implementation, C ABI)

#include <deque>
#include <mutex>
#include <cstring>
#include <cstdint>
#include <algorithm>

extern "C" {
#include "SwiftX11Bridge.h"
}

// xxx temp
#include "WindowTable.hpp"

namespace {

// Keep this reasonably large; it’s cheap.
// If full, we drop newest (safe) and count drops.
constexpr size_t kMaxCmds = 4096;

std::mutex g_mu;
std::deque<x11_ui_cmd_t> g_q;
uint64_t g_push_drops = 0;

static inline void clamp_wh(int32_t& w, int32_t& h) {
  if (w < 1) w = 1;
  if (h < 1) h = 1;
}

// Overwrite last command if same xid+type.
// Also coalesce DAMAGE by unioning rects (same xid).
static inline bool coalesce_tail(x11_ui_cmd_t& tail, const x11_ui_cmd_t& in) {
  if (tail.xid != in.xid) return false;

  if (tail.type == in.type) {
    switch (in.type) {
      case X11_UI_TITLE:
        tail.title_len = in.title_len;
        std::memcpy(tail.title_utf8, in.title_utf8, sizeof(tail.title_utf8));
        return true;

      case X11_UI_SET_CURSOR:
        // last one wins
        tail.cursor = in.cursor;
        return true;
        
      case X11_UI_RESIZE:
        tail.w_u = in.w_u;
        tail.h_u = in.h_u;
        return true;

      case X11_UI_DAMAGE: {
        // union rectangles
        const int32_t ax0 = tail.x_u;
        const int32_t ay0 = tail.y_u;
        const int32_t ax1 = tail.x_u + tail.w_u;
        const int32_t ay1 = tail.y_u + tail.h_u;

        const int32_t bx0 = in.x_u;
        const int32_t by0 = in.y_u;
        const int32_t bx1 = in.x_u + in.w_u;
        const int32_t by1 = in.y_u + in.h_u;

        const int32_t ux0 = std::min(ax0, bx0);
        const int32_t uy0 = std::min(ay0, by0);
        const int32_t ux1 = std::max(ax1, bx1);
        const int32_t uy1 = std::max(ay1, by1);

        tail.x_u = ux0;
        tail.y_u = uy0;
        tail.w_u = std::max<int32_t>(1, ux1 - ux0);
        tail.h_u = std::max<int32_t>(1, uy1 - uy0);
        return true;
      }

      // These are idempotent-ish; overwrite/drop is fine.
      case X11_UI_MAP:
      case X11_UI_UNMAP:
      case X11_UI_RAISE:
      case X11_UI_DESTROY:
        return true;

      default:
        return false;
    }
  }

  // Small cross-type coalescing:
  // - A DESTROY makes earlier TITLE/RESIZE/DAMAGE irrelevant if it’s at the tail.
  if (in.type == X11_UI_DESTROY) {
    // If tail is something about same xid, replace it with destroy.
    tail = in;
    return true;
  }

  return false;
}

static inline x11_ui_cmd_t make_empty() {
  x11_ui_cmd_t c{};
  c.type = X11_UI_NONE;
  return c;
}

static inline void push_cmd(const x11_ui_cmd_t& c) {
  std::lock_guard<std::mutex> lock(g_mu);

  if (!g_q.empty()) {
    if (coalesce_tail(g_q.back(), c)) return;
  }

  if (g_q.size() >= kMaxCmds) {
    g_push_drops++;
    return;
  }
  g_q.push_back(c);
}

static inline void fill_title(x11_ui_cmd_t& c, const char* title_utf8) {
  if (!title_utf8) title_utf8 = "";
  // Cap at 32 bytes (struct fixed)
  size_t n = strnlen(title_utf8, sizeof(c.title_utf8));
  c.title_len = (uint8_t)n;
  std::memset(c.title_utf8, 0, sizeof(c.title_utf8));
  if (n) std::memcpy(c.title_utf8, title_utf8, n);
}

} // namespace

extern "C" bool x11_ui_pop_command(x11_ui_cmd_t* out_cmd) {
  if (!out_cmd) return false;

  std::lock_guard<std::mutex> lock(g_mu);
  if (g_q.empty()) return false;

  *out_cmd = g_q.front();
  g_q.pop_front();
  return true;
}

extern "C" void x11_ui_push_title(uint32_t xid, const char* title_utf8) {
  if (xid == 0) return;
  x11_ui_cmd_t c = make_empty();
  c.type = X11_UI_TITLE;
  c.xid = xid;
  fill_title(c, title_utf8);
  push_cmd(c);
}

extern "C" void x11_ui_push_set_cursor(uint32_t host_xid, uint32_t cursor_xid, int32_t shape) {
  if (host_xid == 0) return;

#ifdef X11_TRACE_VERBOSE
  fprintf(stderr, "[UI_PUSH] SET_CURSOR host=0x%08X cursor=0x%08X shape=%d\n",
          (unsigned)host_xid, (unsigned)cursor_xid, (int)shape);
#endif

  x11_ui_cmd_t c = make_empty();
  c.type = X11_UI_SET_CURSOR;

  // Keep legacy “xid” meaningful for routing/debug
  c.xid = host_xid;

  // Fill explicit cursor payload
  c.cursor.host_xid   = host_xid;
  c.cursor.cursor_xid = cursor_xid;
  c.cursor.shape      = shape;
  c.cursor.reserved   = 0;

  push_cmd(c);
}

extern "C" void x11_ui_push_raise(uint32_t xid) {
  if (xid == 0) return;
  x11_ui_cmd_t c = make_empty();
  c.type = X11_UI_RAISE;
  c.xid = xid;
  push_cmd(c);
}

extern "C" void x11_ui_push_map(uint32_t xid) {
  if (xid == 0) return;
  x11_ui_cmd_t c = make_empty();
  c.type = X11_UI_MAP;
  c.xid = xid;
  push_cmd(c);
}

extern "C" void x11_ui_push_unmap(uint32_t xid) {
  if (xid == 0) return;
  x11_ui_cmd_t c = make_empty();
  c.type = X11_UI_UNMAP;
  c.xid = xid;
  push_cmd(c);
}

extern "C" void x11_ui_push_resize(uint32_t xid, int32_t w_u, int32_t h_u) {
  if (xid == 0) return;
  clamp_wh(w_u, h_u);

  x11_ui_cmd_t c = make_empty();
  c.type = X11_UI_RESIZE;
  c.xid = xid;
  c.w_u = w_u;
  c.h_u = h_u;
  push_cmd(c);
}

extern "C" void x11_ui_push_create(uint32_t xid, uint32_t parent_xid,
                                   int32_t x_u, int32_t y_u,
                                   int32_t w_u, int32_t h_u,
                                   uint32_t flags) {
  if (xid == 0) return;
  clamp_wh(w_u, h_u);

  x11_ui_cmd_t c = make_empty();
  c.type = X11_UI_CREATE;
  c.xid = xid;
  c.parent_xid = parent_xid;
  c.x_u = x_u;
  c.y_u = y_u;
  c.w_u = w_u;
  c.h_u = h_u;
  c.flags = flags;
  push_cmd(c);
}

extern "C" void x11_ui_push_destroy(uint32_t xid) {
  if (xid == 0) return;
  x11_ui_cmd_t c = make_empty();
  c.type = X11_UI_DESTROY;
  c.xid = xid;
  push_cmd(c);
}

extern "C" void x11_ui_push_damage(uint32_t xid, int32_t x_u, int32_t y_u, int32_t w_u, int32_t h_u) {
  if (xid == 0) return;
  clamp_wh(w_u, h_u);

#ifdef X11_TRACE_VERBOSE
  fprintf(stderr, "[UI_DAMAGE] xid=0x%08X rect=(%d,%d %dx%d)\n",
          (unsigned)xid, (int)x_u, (int)y_u, (int)w_u, (int)h_u);
#endif
  x11_ui_cmd_t c = make_empty();
  c.type = X11_UI_DAMAGE;
  c.xid = xid;
  c.x_u = x_u;
  c.y_u = y_u;
  c.w_u = w_u;
  c.h_u = h_u;
  push_cmd(c);
}

extern "C" void x11_ui_push_warp_pointer(uint32_t host_xid, int32_t x, int32_t y) {
  x11_ui_cmd_t c = make_empty();
  c.type = X11_UI_WARP_POINTER;
  c.xid = host_xid;  // 0 means root-relative (screen coords)
  c.x_u = x;
  c.y_u = y;
  push_cmd(c);
}

extern "C" void x11_ui_push_shape_changed(uint32_t host_xid) {
  x11_ui_cmd_t c = make_empty();
  c.type = X11_UI_SHAPE_CHANGED;
  c.xid = host_xid;
  push_cmd(c);
}

// =============================================================================
// Shared damage accumulator — bypasses UI command queue latency.
// C++ writes here from the server thread; Swift reads at present time.
// =============================================================================
namespace {

struct SharedDamageEntry {
  uint32_t host_xid = 0;
  int32_t  x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool     valid = false;
};

constexpr size_t kMaxSharedDamageHosts = 64;
std::mutex g_sd_mu;
SharedDamageEntry g_sd_entries[kMaxSharedDamageHosts];

} // namespace

extern "C" void x11_shared_damage_union(uint32_t host_xid,
                                        int32_t x, int32_t y,
                                        int32_t w, int32_t h) {
  if (host_xid == 0 || w <= 0 || h <= 0) return;

  const int32_t nx1 = x + w;
  const int32_t ny1 = y + h;

  std::lock_guard<std::mutex> lock(g_sd_mu);

  // Find existing entry for this host
  for (size_t i = 0; i < kMaxSharedDamageHosts; i++) {
    auto& e = g_sd_entries[i];
    if (e.host_xid == host_xid) {
      if (e.valid) {
        if (x   < e.x0) e.x0 = x;
        if (y   < e.y0) e.y0 = y;
        if (nx1 > e.x1) e.x1 = nx1;
        if (ny1 > e.y1) e.y1 = ny1;
      } else {
        e.x0 = x;  e.y0 = y;
        e.x1 = nx1; e.y1 = ny1;
        e.valid = true;
      }
      return;
    }
    if (e.host_xid == 0) {
      // Empty slot — claim it
      e.host_xid = host_xid;
      e.x0 = x;  e.y0 = y;
      e.x1 = nx1; e.y1 = ny1;
      e.valid = true;
      return;
    }
  }
  // Table full — silently drop (extremely unlikely with 64 slots)
}

extern "C" bool x11_shared_damage_consume(uint32_t host_xid,
                                          int32_t* out_x, int32_t* out_y,
                                          int32_t* out_w, int32_t* out_h) {
  if (host_xid == 0) return false;

  std::lock_guard<std::mutex> lock(g_sd_mu);

  for (size_t i = 0; i < kMaxSharedDamageHosts; i++) {
    auto& e = g_sd_entries[i];
    if (e.host_xid == host_xid) {
      if (!e.valid) return false;
      if (out_x) *out_x = e.x0;
      if (out_y) *out_y = e.y0;
      if (out_w) *out_w = e.x1 - e.x0;
      if (out_h) *out_h = e.y1 - e.y0;
      e.valid = false;
      e.x0 = e.y0 = e.x1 = e.y1 = 0;
      return true;
    }
  }
  return false;
}

extern "C" void x11_shared_damage_clear(uint32_t host_xid) {
  if (host_xid == 0) return;

  std::lock_guard<std::mutex> lock(g_sd_mu);

  for (size_t i = 0; i < kMaxSharedDamageHosts; i++) {
    auto& e = g_sd_entries[i];
    if (e.host_xid == host_xid) {
      e = SharedDamageEntry{};  // clear slot for reuse
      return;
    }
  }
}
