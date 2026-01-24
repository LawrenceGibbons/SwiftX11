//
//  XProtoServerBridge.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/21/26.
//

#include <atomic>
#include <mutex>
#include <cstddef>
#include <cstdio>
#include <array>
#include <cstring>

#include "XProtoServerBridge.h"

#include "XProtoServer.hpp"        // owns ctx_, eventOps_, transport_
#include "XProtoWindowLookupBridge.h"

static std::mutex g_mu; // only used to serialize begin/end session
static std::atomic<x11::XProtoServer*> g_srv{nullptr};

static bool c_lookup_window(uint32_t xid, x11::WindowView* out, void* /*user*/)
{
  if (!out) return false;

  int16_t x=0,y=0;
  uint16_t w=0,h=0;
  uint32_t mask=0;
  int mapped=0;
  int owner_fd=-1;

  if (!x11_xproto_snapshot_window_view(xid, &x, &y, &w, &h, &mask, &mapped, &owner_fd)) {
    return false;
  }

  out->xid = xid;
  out->x = x;
  out->y = y;
  out->w = w;
  out->h = h;
  out->event_mask = mask;
  out->mapped = (mapped != 0);
  out->owner_fd = owner_fd;
  return true;
}


extern "C" void x11_proto_bridge_begin_session(int client_fd)
{
  std::lock_guard<std::mutex> lock(g_mu);
  if (client_fd < 0) return;

  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) {
    srv = new x11::XProtoServer();
    g_srv.store(srv, std::memory_order_release);
  }
  srv->setWindowLookup(&c_lookup_window, nullptr);
  srv->attachClientFd(client_fd);
  srv->setXprotoThreadSelf();
}

extern "C" void x11_proto_bridge_end_session(void)
{
  std::lock_guard<std::mutex> lock(g_mu);
  auto* srv = g_srv.exchange(nullptr, std::memory_order_acq_rel);
  delete srv;
}

extern "C" void x11_proto_bridge_note_last_seq(uint16_t seq)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (srv) srv->noteLastSeq(seq);
}

extern "C" void x11_proto_bridge_flush_notify_queue(void)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (srv) srv->flushNotifyQueue();
}

extern "C" void x11_proto_bridge_queue_notify(uint32_t wid, int want_configure, int want_expose)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  srv->queueNotify(wid, want_configure != 0, want_expose != 0);
}

extern "C" void x11_proto_bridge_queue_expose_rect(uint32_t wid,
                                                   uint16_t x, uint16_t y,
                                                   uint16_t w, uint16_t h,
                                                   uint16_t count) {
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  srv->transport().queueExposeRect(wid, x, y, w, h, count);
}

extern "C" int x11_proto_bridge_send_reply_bytes(const void* buf, size_t n)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return 0;
  if (!buf || n == 0) return 0;

  // Must be on xproto thread; transport enforces that.
  return srv->transport().sendReplyBytes(buf, n) ? 1 : 0;
}

extern "C" int x11_proto_bridge_send_get_geometry_reply(uint16_t seq,
                                                        uint32_t root,
                                                        int16_t x, int16_t y,
                                                        uint16_t w, uint16_t h,
                                                        uint16_t borderWidth,
                                                        uint16_t depth)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return 0;

  // Forward to the unified ReplyWriter path.
  return srv->ctx().reply().sendGetGeometryReply(seq, root, x, y, w, h, borderWidth, depth) ? 1 : 0;
}

extern "C" int x11_proto_bridge_send_get_input_focus_reply(uint16_t seq,
                                                           uint8_t revertTo,
                                                           uint32_t focus)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return 0;

  return srv->ctx().reply().sendGetInputFocusReply(seq, revertTo, focus) ? 1 : 0;
}


extern "C" int x11_proto_bridge_send_intern_atom_reply(uint16_t seq, uint32_t atom) {
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return 0;
  return srv->ctx().reply().sendInternAtomReply(seq, atom) ? 1 : 0;
}

extern "C" int x11_proto_bridge_send_get_atom_name_reply(uint16_t seq, const char* name, uint16_t nameLen) {
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return 0;
  return srv->ctx().reply().sendGetAtomNameReply(seq, name, nameLen) ? 1 : 0;
}

extern "C" void x11_proto_bridge_send_query_pointer_reply(uint16_t seq,
                                                          uint8_t sameScreen,
                                                          uint32_t root,
                                                          uint32_t child,
                                                          int16_t rootX, int16_t rootY,
                                                          int16_t winX, int16_t winY,
                                                          uint16_t mask)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;

  // Build the 32-byte reply
  srv->ctx().reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = sameScreen; // sameScreen

    // root @ 8..11
    rep[8]  = (uint8_t)(root & 0xFF);
    rep[9]  = (uint8_t)((root >> 8) & 0xFF);
    rep[10] = (uint8_t)((root >> 16) & 0xFF);
    rep[11] = (uint8_t)((root >> 24) & 0xFF);

    // child @ 12..15
    rep[12] = (uint8_t)(child & 0xFF);
    rep[13] = (uint8_t)((child >> 8) & 0xFF);
    rep[14] = (uint8_t)((child >> 16) & 0xFF);
    rep[15] = (uint8_t)((child >> 24) & 0xFF);

    // rootX/rootY/winX/winY are INT16 on wire; store as little-endian uint16_t
    auto wr16 = [&](size_t off, uint16_t v) {
      rep[off + 0] = (uint8_t)(v & 0xFF);
      rep[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    };

    wr16(16, (uint16_t)rootX);
    wr16(18, (uint16_t)rootY);
    wr16(20, (uint16_t)winX);
    wr16(22, (uint16_t)winY);

    wr16(24, mask); // mask
  });
}

extern "C" void x11_proto_bridge_send_query_tree_reply_header(uint16_t seq,
                                                              uint32_t root,
                                                              uint32_t parent,
                                                              uint16_t nchildren)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;

  // extra_words = nchildren (each child is CARD32)
  const uint32_t extra_words = (uint32_t)nchildren;

  srv->ctx().reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    // length_words @ 4..7
    rep[4] = (uint8_t)(extra_words & 0xFF);
    rep[5] = (uint8_t)((extra_words >> 8) & 0xFF);
    rep[6] = (uint8_t)((extra_words >> 16) & 0xFF);
    rep[7] = (uint8_t)((extra_words >> 24) & 0xFF);

    // root @ 8..11
    rep[8]  = (uint8_t)(root & 0xFF);
    rep[9]  = (uint8_t)((root >> 8) & 0xFF);
    rep[10] = (uint8_t)((root >> 16) & 0xFF);
    rep[11] = (uint8_t)((root >> 24) & 0xFF);

    // parent @ 12..15
    rep[12] = (uint8_t)(parent & 0xFF);
    rep[13] = (uint8_t)((parent >> 8) & 0xFF);
    rep[14] = (uint8_t)((parent >> 16) & 0xFF);
    rep[15] = (uint8_t)((parent >> 24) & 0xFF);

    // nchildren @ 16..17
    rep[16] = (uint8_t)(nchildren & 0xFF);
    rep[17] = (uint8_t)((nchildren >> 8) & 0xFF);
  });
}

extern "C" void x11_proto_bridge_send_query_tree_children(const uint32_t* children,
                                                          uint16_t nchildren)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  if (!children || nchildren == 0) return;

  // children list is CARD32[] in little-endian
  // We’ll pack into a temporary byte buffer.
  // Cap at 256 to mirror your current stack allocation pattern.
  if (nchildren > 256) nchildren = 256;

  uint8_t out[256 * 4];
  for (uint16_t i = 0; i < nchildren; i++) {
    const uint32_t v = children[i];
    out[i * 4 + 0] = (uint8_t)(v & 0xFF);
    out[i * 4 + 1] = (uint8_t)((v >> 8) & 0xFF);
    out[i * 4 + 2] = (uint8_t)((v >> 16) & 0xFF);
    out[i * 4 + 3] = (uint8_t)((v >> 24) & 0xFF);
  }

  // Children list is already 4-byte aligned, so sendReplyBytes is fine.
  srv->transport().sendReplyBytes(out, (size_t)nchildren * 4u);
}


