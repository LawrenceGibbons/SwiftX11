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
#include "QueryOps.hpp"   // (and later AtomOps.hpp, WindowOps.hpp, etc.)
#include "WindowTable.hpp"
#include "XProtoModules.hpp"
#include "XProtoContext.hpp"
#include "x11_window_set_mapped.h"
#include "GCTable.hpp"
#include "XProtoGCBridge.hpp"

// Modules live for the lifetime of the session.
static std::atomic<x11::XProtoModules*> g_mods{nullptr};
static std::mutex g_mu; // only used to serialize begin/end session
static std::atomic<x11::XProtoServer*> g_srv{nullptr};

static bool c_lookup_window(uint32_t xid, x11::WindowView* out, void* /*user*/)
{
  fprintf(stderr, "[BRIDGE] c_lookup_window xid=0x%08X\n", (unsigned)xid);
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
  
  // 1) Create server once per session (or reuse if you decide to support reuse).
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) {
    srv = new x11::XProtoServer();
    g_srv.store(srv, std::memory_order_release);
  }
  
  // 2) Configure server plumbing for THIS session.
  //srv->setWindowLookup(&c_lookup_window, nullptr);
  srv->attachClientFd(client_fd);
  srv->setXprotoThreadSelf();
  
  // 3) Create modules ONCE per session; constructors register opcode handlers into srv.
  //    Important: create modules AFTER srv exists, because they register into it.
  auto* mods = g_mods.load(std::memory_order_acquire);
  if (!mods) {
    mods = new x11::XProtoModules(*srv); // *srv is the registrar
    g_mods.store(mods, std::memory_order_release);
  }
}


extern "C" void x11_proto_bridge_end_session(void)
{
  std::lock_guard<std::mutex> lock(g_mu);

  // Destroy modules first (they may reference the server during destruction in the future).
  auto* mods = g_mods.exchange(nullptr, std::memory_order_acq_rel);
  delete mods;

  // Then destroy the server.
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

//extern "C" int x11_proto_bridge_send_get_geometry_reply(uint16_t seq,
//                                                        uint32_t root,
//                                                        int16_t x, int16_t y,
//                                                        uint16_t w, uint16_t h,
//                                                        uint16_t borderWidth,
//                                                        uint16_t depth)
//{
//  auto* srv = g_srv.load(std::memory_order_acquire);
//  if (!srv) return 0;
//
//  // Forward to the unified ReplyWriter path.
//  return srv->ctx().reply().sendGetGeometryReply(seq, root, x, y, w, h, borderWidth, depth) ? 1 : 0;
//}

// extern "C" int x11_proto_bridge_send_get_input_focus_reply(uint16_t seq,
//                                                            uint8_t revertTo,
//                                                            uint32_t focus)
// {
//   auto* srv = g_srv.load(std::memory_order_acquire);
//   if (!srv) return 0;
// 
//   return srv->ctx().reply().sendGetInputFocusReply(seq, revertTo, focus) ? 1 : 0;
// }


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


extern "C" int x11_proto_bridge_dispatch(uint8_t major, uint8_t minor, uint16_t seq,
                                         const uint8_t* payload, size_t remain)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return 0;
  return srv->dispatch(major, minor, seq, payload, remain) ? 1 : 0;
}


//extern "C" void x11_proto_bridge_window_upsert(uint32_t xid, uint32_t parent,
//                                               int16_t x, int16_t y,
//                                               uint16_t w, uint16_t h,
//                                               uint32_t event_mask,
//                                               int owner_fd)
//{
//  auto* srv = g_srv.load(std::memory_order_acquire);
//  if (!srv) return;
//  srv->ctx().windows().upsert(xid, parent, x, y, w, h, event_mask, owner_fd);
//}

extern "C" void x11_proto_bridge_window_erase(uint32_t xid)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  srv->ctx().windows().erase(xid);
}

extern "C" void x11_proto_bridge_window_set_mapped(uint32_t xid, int mapped)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  srv->ctx().windows().setMapped(xid, mapped != 0);
}

extern "C" void x11_proto_bridge_window_set_presentable(uint32_t xid, int presentable)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  srv->ctx().windows().setPresentable(xid, presentable != 0);
  x11_xproto_c_window_set_presentable(xid, presentable != 0);
}

extern "C" void x11_proto_bridge_window_set_event_mask(uint32_t xid, uint32_t event_mask)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  srv->ctx().windows().setEventMask(xid, event_mask);
  fprintf(stderr, "[BRIDGE] set_event_mask xid=0x%08X mask=0x%08X\n", xid, event_mask);
}

extern "C" void x11_proto_bridge_window_set_geometry(uint32_t xid, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  srv->ctx().windows().setGeometry(xid, x, y, w, h);
}

extern "C" int x11_proto_bridge_window_is_ready_to_present(uint32_t xid)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return 0;
  return srv->ctx().windows().isReadyToPresent(xid) ? 1 : 0;
}

extern "C" void x11_proto_bridge_window_mark_dirty(uint32_t xid)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  srv->ctx().windows().markDirty(xid);
}

extern "C" int x11_proto_bridge_window_consume_dirty_if_ready(uint32_t xid)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return 0;
  return srv->ctx().windows().consumeDirtyIfReady(xid) ? 1 : 0;
}

extern "C" void x11_proto_bridge_window_debug_state(uint32_t xid,
                                                    uint32_t* out_parent,
                                                    int* out_mapped,
                                                    int* out_presentable,
                                                    int* out_dirty,
                                                    int* out_owner_fd)
{
  if (out_parent)      *out_parent = 0;
  if (out_mapped)      *out_mapped = 0;
  if (out_presentable) *out_presentable = 0;
  if (out_dirty)       *out_dirty = 0;
  if (out_owner_fd)    *out_owner_fd = -1;

  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;

  x11::WindowView vw{};
  if (!srv->ctx().windows().snapshot(xid, vw)) {
    // fallback to old callback snapshot if you want:
    // if (!srv->ctx().snapshotViaCallback(xid, vw)) return;
    return;
  }

  if (out_parent)      *out_parent = vw.parent_xid;     // see note below
  if (out_mapped)      *out_mapped = vw.mapped ? 1 : 0;
  if (out_presentable) *out_presentable = vw.presentable ? 1 : 0; // see note below
  if (out_dirty)       *out_dirty = vw.dirty ? 1 : 0;             // see note below
  if (out_owner_fd)    *out_owner_fd = vw.owner_fd;
}
