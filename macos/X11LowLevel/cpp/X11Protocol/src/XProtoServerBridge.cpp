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
#include <deque>
#include <cstdint>

#include "XProtoServerBridge.h"
extern "C" {
#include "x11_requests.h"
#include "x11_backend_fb.h"   // adjust path to wherever you placed it
}
#include "XProtoServer.hpp"        // owns ctx_, eventOps_, transport_
#include "QueryOps.hpp"   // (and later AtomOps.hpp, WindowOps.hpp, etc.)
#include "WindowTable.hpp"
#include "XProtoModules.hpp"
#include "XProtoContext.hpp"
//#include "x11_window_set_mapped.h"
#include "GCTable.hpp"
#include "XProtoGCBridge.hpp"
#include "HostResize.hpp"
#include "XProtoDaemon.hpp"



// ---- Host-command queue (server thread -> xproto thread) ----
namespace {

enum class HostCmdType : uint8_t {
  RootlessResize,
  SetPresentable,
};

struct HostCmd {
  HostCmdType type;
  uint32_t xid;
  int32_t w_px;
  int32_t h_px;
};

std::mutex g_hostcmd_mu;
std::deque<HostCmd> g_hostcmd_q;

static inline void hostcmd_push(const HostCmd& c) {
  std::lock_guard<std::mutex> lock(g_hostcmd_mu);

  if (c.type == HostCmdType::RootlessResize) {
    if (!g_hostcmd_q.empty()) {
      HostCmd& back = g_hostcmd_q.back();
      if (back.type == HostCmdType::RootlessResize && back.xid == c.xid) {
        back.w_px = c.w_px;
        back.h_px = c.h_px;
        return;
      }
    }
  }

  g_hostcmd_q.push_back(c);
}
  
  
static inline std::deque<HostCmd> hostcmd_take_all() {
  std::lock_guard<std::mutex> lock(g_hostcmd_mu);
  std::deque<HostCmd> out;
  out.swap(g_hostcmd_q);
  return out;
}

} // namespace



// Modules live for the lifetime of the session.
static std::atomic<x11::XProtoModules*> g_mods{nullptr};
static std::mutex g_mu; // only used to serialize begin/end session
static std::atomic<x11::XProtoServer*> g_srv{nullptr};


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


extern "C" void x11_proto_bridge_end_session(int client_fd)
{
  x11::XProtoModules* mods = nullptr;
  x11::XProtoServer*  srv  = nullptr;

  {
    std::lock_guard<std::mutex> lock(g_mu);
    mods = g_mods.exchange(nullptr, std::memory_order_acq_rel);
    srv  = g_srv.exchange(nullptr, std::memory_order_acq_rel);
  }

  if (srv) {
    auto& ctx = srv->ctx();

    // Erase windows owned by this fd (child-first order).
    std::vector<uint32_t> owned = ctx.windows().eraseOwnedBy(client_fd);

    // For each window: free C backing store + tell Swift to destroy the native window.
    for (uint32_t wid : owned) {
      x11_backend_fb_destroy(wid);
      x11_requests_push_destroy(wid);
    }

    // Optional: clear notify queue if you keep server alive across sessions.
    // Since we're deleting srv, per-session notify state is discarded.
  }

  delete mods;
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
  if (!srv) return;
  
  auto& ctx = srv->ctx();

  // ---- drain host commands on xproto thread ----
  {
    auto cmds = hostcmd_take_all();
    for (const auto& c : cmds) {
      switch (c.type) {
        case HostCmdType::RootlessResize:
          // Runs on xproto thread now: safe vs drawing + fb resize
          applyRootlessResize(ctx, c.xid, c.w_px, c.h_px);
          break;

        case HostCmdType::SetPresentable:
          ctx.windows().setPresentable(c.xid, true);
          if (ctx.windows().consumeDirtyIfReady(c.xid)) {
            x11_requests_push_damage(c.xid);
          }
          break;
      }
    }
  }

    
    
    srv->flushNotifyQueue();
}

//extern "C" void x11_proto_bridge_queue_notify(uint32_t wid, int want_configure, int want_expose)
//{
//  auto* srv = g_srv.load(std::memory_order_acquire);
//  if (!srv) return;
//  srv->queueNotify(wid, want_configure != 0, want_expose != 0);
//}

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

extern "C" void x11_proto_bridge_window_set_event_mask(uint32_t xid, uint32_t event_mask)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  srv->ctx().windows().setEventMask(xid, event_mask);
}

//extern "C" void x11_proto_bridge_window_set_geometry(uint32_t xid, int16_t x, int16_t y, uint16_t w, uint16_t h)
//{
//  auto* srv = g_srv.load(std::memory_order_acquire);
//  if (!srv) return;
//  srv->ctx().windows().setGeometry(xid, x, y, w, h);
//}

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

extern "C" void x11_proto_bridge_pixmap_create(uint32_t pid, uint8_t depth, uint16_t w, uint16_t h)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  srv->ctx().pixmaps().createPixmap(pid, depth, w, h);
}

extern "C" void x11_proto_bridge_pixmap_free(uint32_t pid)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (!srv) return;
  srv->ctx().pixmaps().freePixmap(pid);
}

extern "C" void x11_proto_bridge_apply_rootless_resize(uint32_t xid, int32_t w_px, int32_t h_px)
{
  if (xid == 0) return;
  if (w_px < 1) w_px = 1;
  if (h_px < 1) h_px = 1;

  hostcmd_push(HostCmd{HostCmdType::RootlessResize, xid, w_px, h_px});
}

extern "C" void x11_proto_bridge_window_set_presentable_and_flush(uint32_t xid)
{
  if (xid == 0) return;
  hostcmd_push(HostCmd{HostCmdType::SetPresentable, xid, 0, 0});
}


static x11::XProtoDaemon g_daemon;

extern "C" int x11_proto_start_daemon(int display)
{
  return g_daemon.start(display) ? 1 : 0;
}

extern "C" void x11_proto_stop_daemon(void)
{
  g_daemon.stop();
}


