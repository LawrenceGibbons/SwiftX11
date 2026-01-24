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

