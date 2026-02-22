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
#include "Core/XProtoServer.hpp"        // owns ctx_, eventOps_, transport_
#include "Ops/QueryOps.hpp"   // (and later AtomOps.hpp, WindowOps.hpp, etc.)
#include "Core/WindowTable.hpp"
#include "Core/XProtoModules.hpp"
#include "Core/XProtoContext.hpp"
//#include "x11_window_set_mapped.h"
#include "Core/GCTable.hpp"
#include "Core/HostResize.hpp"
#include "Transport/XProtoDaemon.hpp"
#include "Core/WindowView.hpp"
#include "Ops/EventOps.hpp"
#include "XProtoNotifyBridge.hpp"
#include "Core/XEventMask.hpp"
#include "Core/X11Modifiers.hpp"
#include "Core/InputRouting.hpp"
#include "Core/timestamp.hpp"
#include "SwiftX11Bridge.h"
#include "WireEvents.hpp"
#include "Core/CursorRouting.hpp"

// ---- Host-command queue (server thread -> xproto thread) ----
namespace {

  enum class HostCmdType : uint8_t {
    RootlessResize,
    SetPresentable,
    PointerMove,
    PointerEnter,
    PointerLeave,
    Button,
    ScrollTicks,
    Key,
    Focus,
  };
  
  struct HostCmd {
    HostCmdType type;

    uint32_t xid = 0;

    // window resizing
    int32_t w_px = 0;
    int32_t h_px = 0;

    // pointer
    int32_t win_x_u = 0;    // X11 units, not pixels
    int32_t win_y_u = 0;    // X11 units, not pixels
    int32_t root_x_u = 0;   // X11 units, not pixels
    int32_t root_y_u = 0;   // X11 units, not pixels
    uint8_t deliver = 0; // 1 => deliver MotionNotify, 0 => only update InputState

    uint32_t buttonsMask = 0;
    uint32_t modsMask = 0;

    // buttons / scroll / keys
    uint8_t button = 0;
    uint8_t isDown = 0;
    int16_t ticks = 0;
    uint8_t axis = 0;
    uint32_t keyCode = 0;
    // (don’t keep utf8 across threads yet)
    
    uint8_t focused = 0;
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


extern "C" void x11_cpp_notify_init(void* ctx_ptr, void* event_ops_ptr, void* queue_ptr);
extern "C" void x11_cpp_notify_shutdown(void); 

extern "C" void x11_proto_bridge_begin_session(int client_fd,
                                               uint32_t rid_base,
                                               uint32_t rid_mask)
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
  srv->transport().setClientIdSpace(rid_base, rid_mask);
  
  srv->attachClientFd(client_fd);
  srv->setXprotoThreadSelf();
  
  // 3) Create modules ONCE per session; constructors register opcode handlers into srv.
  //    Important: create modules AFTER srv exists, because they register into it.
  auto* mods = g_mods.load(std::memory_order_acquire);
  if (!mods) {
    mods = new x11::XProtoModules(*srv); // *srv is the registrar
    g_mods.store(mods, std::memory_order_release);
  }
  
  // 4) Initialize the notify pointers
  if ( srv ) {
    void* ctx_ptr = (void*)&srv->ctx();
    void* ev_ptr  = (void*)&srv->eventOps();
    void* q_ptr   = (void*)&srv->ctx().transport().notifyQueue(); 
    
    x11_cpp_notify_init(ctx_ptr, ev_ptr, q_ptr);
  }
}


extern "C" void x11_proto_bridge_end_session(int client_fd)
{
  x11_cpp_notify_shutdown();
  
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


extern "C" x11::XProtoServer* x11_proto_bridge_get_server(void)
{
  return g_srv.load(std::memory_order_acquire);
}


extern "C" void x11_proto_bridge_note_last_seq(uint16_t seq)
{
  auto* srv = g_srv.load(std::memory_order_acquire);
  if (srv) srv->noteLastSeq(seq);
}


static uint32_t pickButtonDeliveryWindow(x11::XProtoContext& ctx,
                                         uint32_t host,
                                         uint32_t under,
                                         bool wantPress,
                                         bool wantRelease)
{
  auto wants = [&](uint32_t xid) -> bool {
    if (!xid) return false;
    const x11::WindowView* vw = ctx.window(xid);
    if (!vw || vw->owner_fd <= 0) return false;
    const uint32_t mask = vw->event_mask;
    const bool wp = (mask & x11::mask::ButtonPress) != 0;
    const bool wr = (mask & x11::mask::ButtonRelease) != 0;
    return (!wantPress || wp) && (!wantRelease || wr);
  };

  uint32_t cur = under ? under : host;
  int safety = 0;

  while (cur) {
    if (wants(cur)) return cur;
    if (cur == host) break;

    x11::WindowView vw{};
    if (!ctx.windows().snapshot(cur, vw)) break;
    cur = vw.parent_xid;

    if (++safety > 64) break;
  }

  // Fall back to host if it wants the events
  if (host && wants(host)) return host;
  return under ? under : host;
}

// xxx temp ---
static inline void sendExposeNow(x11::XProtoContext& ctx,
                                 x11::EventOps& evOps,
                                 uint32_t wid)
{
  const x11::WindowView* wv = ctx.window(wid);
  if (!wv) return;

  auto ev = x11::wireev::buildExpose(ctx.transport().lastSeq(),
                                   wid,
                                   0, 0,
                                   wv->w, wv->h,
                                   0);
  ctx.transport().sendEvent32(wid, ev.data());
}
// xxx --- temp


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
        // ------------------- RootlessResize  
        case HostCmdType::RootlessResize:
          // Runs on xproto thread now: safe vs drawing + fb resize
          applyRootlessResize(ctx, c.xid, c.w_px, c.h_px);
          break;
          
        // ------------------- SetPresentable
        case HostCmdType::SetPresentable:
          ctx.windows().setPresentable(c.xid, true);
          if (ctx.windows().consumeDirtyIfReady(c.xid)) {
            x11_requests_push_damage(c.xid);
          }
          break;
          
        // ------------------- PointerMove
        case HostCmdType::PointerMove: {
          x11::notify::postMotion(c.xid,
                                  c.win_x_u, c.win_y_u,
                                  c.root_x_u, c.root_y_u,
                                  c.deliver,
                                  c.buttonsMask, c.modsMask);
          break;
        }
          
          
        // ------------------- PointerEnter
        case HostCmdType::PointerEnter: {
          const uint32_t host = c.xid ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // Find what we're entering *within* this host, using existing host-local coords.
          uint32_t under = x11::pickDeepestMappedWindowAtHostPoint(ctx, host,
                                                                  ctx.input().win_x_u,
                                                                  ctx.input().win_y_u);
          if (!under) under = host;

          // Pointer ownership should be the window under the pointer, not the host.
          ctx.input().enter(under);

          // Apply cursor to the Cocoa host window, choosing cursor from the routed pointer target.
          const uint32_t cursorTarget = ctx.input().routePointer(under);
          maybeApplyCursor(ctx, host, cursorTarget);

          srv->eventOps().sendCrossingEvent(ctx, under, /*is_enter=*/true,
                                            ctx.input().root_x_u, ctx.input().root_y_u,
                                            ctx.input().buttons, c.modsMask);
          break;
        }
          
          
        // ------------------- PointerLeave
        case HostCmdType::PointerLeave: {
          const uint32_t host = c.xid ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // Window that currently "has" the pointer (drag wins).
          uint32_t leaveWin = 0;
          if (ctx.input().drag_xid)       leaveWin = ctx.input().drag_xid;
          else if (ctx.input().pointer_xid) leaveWin = ctx.input().pointer_xid;
          else                             leaveWin = host;

          // Update pointer ownership state.
          ctx.input().leave(leaveWin);

          // After leaving, cursor should usually fall back (focus/host/inherit).
          const uint32_t cursorTarget = ctx.input().routePointer(host);
          maybeApplyCursor(ctx, host, cursorTarget);

          srv->eventOps().sendCrossingEvent(ctx, leaveWin, /*is_enter=*/false,
                                            ctx.input().root_x_u, ctx.input().root_y_u,
                                            ctx.input().buttons, c.modsMask);
          break;
        }          
          
          
        // ------------------- Focus
        case HostCmdType::Focus: {
          const uint32_t host = c.xid;
          if (!host) break;

          const uint32_t oldFocus = ctx.input().focus_xid;

          auto isMappedWin = [&](uint32_t xid) -> bool {
            if (!xid) return false;
            x11::WindowView vw{};
            if (!ctx.windows().snapshot(xid, vw)) return false;
            return vw.mapped;
          };

          auto belongsToHost = [&](uint32_t xid) -> bool {
            return xid && (ctx.windows().topLevelAncestorOf(xid) == host);
          };

          if (c.focused) {
            ctx.input().focus_host = host;

            // 0) Prefer keeping the existing focus if it still belongs to this host and is mapped.
            uint32_t target = 0;
            if (belongsToHost(oldFocus) && isMappedWin(oldFocus)) {
              target = oldFocus;
            }

            // 1) Otherwise, prefer pointer owner if it belongs to this host, is mapped, and isn't just host.
            if (!target) {
              uint32_t p = ctx.input().pointer_xid;
              if (p != 0 && p != host && belongsToHost(p) && isMappedWin(p)) {
                target = p;
              }
            }

            // 2) Otherwise choose deepest mapped under current host-local pointer.
            if (!target) {
              target = x11::pickDeepestMappedWindowAtHostPoint(ctx, host,
                                                               ctx.input().win_x_u,
                                                               ctx.input().win_y_u);
              if (target && !isMappedWin(target)) target = 0;
            }

            // 3) Last resort: focus the host itself.
            if (!target) target = host;

            // Update focus bookkeeping.
            ctx.input().focus_xid = target;

            // Pointer_xid follow-focus only when not dragging/grabbing (keep your behavior).
            if (ctx.input().drag_xid == 0) ctx.input().pointer_xid = target;

            // ---- CRITICAL: emit FocusOut/FocusIn to clients ----
            // Only send FocusOut if oldFocus was real and on this host.
            if (oldFocus && oldFocus != target && belongsToHost(oldFocus)) {
              srv->eventOps().sendFocusEvent(ctx, oldFocus, /*is_in=*/false);
            }
            // FocusIn for new target (host or child).
            srv->eventOps().sendFocusEvent(ctx, target, /*is_in=*/true);

        #ifndef NDEBUG
            fprintf(stderr, "[FOCUS] host=0x%08X old=0x%08X new=0x%08X pointer_xid=0x%08X win=(%d,%d)\n",
                    (unsigned)host,
                    (unsigned)oldFocus,
                    (unsigned)ctx.input().focus_xid,
                    (unsigned)ctx.input().pointer_xid,
                    (int)ctx.input().win_x_u,
                    (int)ctx.input().win_y_u);
        #endif

          } else {
            // Losing focus on this host: emit FocusOut for current focus if it belongs to this host.
            if (ctx.input().focus_host == host) ctx.input().focus_host = 0;

            if (oldFocus && belongsToHost(oldFocus)) {
              srv->eventOps().sendFocusEvent(ctx, oldFocus, /*is_in=*/false);
            }

            ctx.input().focus_xid = 0;
            if (ctx.input().drag_xid == 0) ctx.input().pointer_xid = 0;

        #ifndef NDEBUG
            fprintf(stderr, "[FOCUS] host=0x%08X lost focus (old=0x%08X)\n",
                    (unsigned)host, (unsigned)oldFocus);
        #endif
          }

          break;
        }

        // ------------------- Button
        case HostCmdType::Button: {
          // Canonicalize buttons + drag grab semantics in InputState
          ctx.input().button(c.xid, c.isDown != 0, c.button, c.buttonsMask);
          ctx.input().mods = c.modsMask;

          const uint32_t host = c.xid ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // If a drag grab is active, route to grab owner (classic behavior).
          // Otherwise, pick the deepest mapped window under the pointer.
          uint32_t under = 0;
          if (ctx.input().drag_xid) {
            under = ctx.input().drag_xid;
          } else {
            under = x11::pickDeepestMappedWindowAtHostPoint(ctx, host,
                                                            ctx.input().win_x_u,
                                                            ctx.input().win_y_u);
            if (!under) under = host;
          }

          // ---- CLICK-TO-FOCUS (this is the key for xterm caret) ----
          if (c.isDown != 0 && c.button == 1) { // left press
            const uint32_t prev = ctx.input().focus_xid;

            // Focus should follow the deepest real window under pointer (NOT "deliver").
            ctx.input().setFocus(host, under);

          #ifndef NDEBUG
            fprintf(stderr,
                    "[FOCUS_SET] host=0x%08X prev=0x%08X new=0x%08X\n",
                    (unsigned)host, (unsigned)prev, (unsigned)under);
          #endif

            if (prev && prev != under) {
              srv->eventOps().sendFocusEvent(ctx, prev, /*is_in=*/false);
            }
            srv->eventOps().sendFocusEvent(ctx, under, /*is_in=*/true);
          }
          
          
          
          // Pick a delivery window that selected the relevant mask.
          auto wantsBtn = [&](uint32_t xid) -> bool {
            if (!xid) return false;
            const x11::WindowView* vw = ctx.window(xid);
            if (!vw || vw->owner_fd <= 0) return false;
            const uint32_t mask = vw->event_mask;
            const bool wantPress   = (mask & x11::mask::ButtonPress) != 0;
            const bool wantRelease = (mask & x11::mask::ButtonRelease) != 0;
            return (c.isDown ? wantPress : wantRelease);
          };

          uint32_t deliver = under;

          // If under doesn't select, climb to parent until host (simple propagation).
          if (!wantsBtn(deliver)) {
            uint32_t cur = under;
            int safety = 0;
            while (cur && cur != host) {
              x11::WindowView vw{};
              if (!ctx.windows().snapshot(cur, vw)) break;
              cur = vw.parent_xid;
              if (wantsBtn(cur)) { deliver = cur; break; }
              if (++safety > 64) break;
            }
            // If still not found, try host last.
            if (!wantsBtn(deliver) && wantsBtn(host)) deliver = host;
          }

          // If nobody wants it, drop.
          if (!wantsBtn(deliver)) break;

          // child field: if delivering to ancestor, child is the subwindow under pointer
          const uint32_t child = (deliver != under) ? under : 0;

        #ifndef NDEBUG
          fprintf(stderr,
                  "[BTN] host=0x%08X under=0x%08X deliver=0x%08X child=0x%08X down=%d btn=%u mods=0x%X\n",
                  (unsigned)host, (unsigned)under, (unsigned)deliver, (unsigned)child,
                  (int)(c.isDown != 0), (unsigned)c.button, (unsigned)c.modsMask);
        #endif

          srv->eventOps().sendButtonEvent(ctx, deliver,
                                          c.isDown != 0, c.button,
                                          ctx.input().root_x_u, ctx.input().root_y_u,
                                          ctx.input().buttons, c.modsMask,
                                          child);
          break;
        }
          
          
        // ------------------- ScrollTicks
        case HostCmdType::ScrollTicks: {
          const uint32_t host = c.xid ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // bring-up: ignore horizontal (axis==1) if desired
          if (c.axis == 1) break;

          const int32_t rx = ctx.input().root_x_u;
          const int32_t ry = ctx.input().root_y_u;

          ctx.input().updateMotion(host,
                                   c.win_x_u, c.win_y_u,
                                   rx, ry,
                                   ctx.input().buttons,
                                   c.modsMask);

          const uint32_t under  = x11::pickDeepestMappedWindowAtHostPoint(ctx, host, c.win_x_u, c.win_y_u);
          const uint32_t target = under ? under : host;

          // Cursor: apply to host, choose based on routed pointer target.
          const uint32_t cursorTarget = ctx.input().routePointer(target);
          maybeApplyCursor(ctx, host, cursorTarget);

          auto wheelButton = [&](uint8_t axis, int16_t ticks) -> uint8_t {
            if (axis == 0) return (ticks > 0) ? 4 : 5; // vertical up/down
            else           return (ticks > 0) ? 6 : 7; // horizontal right/left
          };

          const int16_t ticks = (int16_t)c.ticks;
          const int n   = (ticks >= 0) ? (int)ticks : (int)(-ticks);
          const int dir = (ticks >= 0) ? +1 : -1;

          const int nClamped = (n > 64) ? 64 : n;

          for (int i = 0; i < nClamped; i++) {
            const uint8_t btn = wheelButton(c.axis, (int16_t)dir);

        #ifndef NDEBUG
            fprintf(stderr,
                    "[SCROLL] host=0x%08X target=0x%08X axis=%u ticks=%d btn=%u win=(%d,%d) root=(%d,%d) t=%u\n",
                    (unsigned)host, (unsigned)target,
                    (unsigned)c.axis, (int)ticks, (unsigned)btn,
                    (int)c.win_x_u, (int)c.win_y_u,
                    (int)rx, (int)ry,
                    (unsigned)x11_now_ms_monotonic());
        #endif

            srv->eventOps().sendButtonEvent(ctx, target,
                                            /*is_press=*/true, btn,
                                            rx, ry,
                                            ctx.input().buttons, c.modsMask,
                                            /*child_xid=*/0);

            const uint32_t wheelMask = (btn >= 1 && btn <= 31) ? (1u << (btn - 1u)) : 0;
            srv->eventOps().sendButtonEvent(ctx, target,
                                            /*is_press=*/false, btn,
                                            rx, ry,
                                            (ctx.input().buttons | wheelMask), c.modsMask,
                                            /*child_xid=*/0);
          }

          sendExposeNow(ctx, srv->eventOps(), target);
          break;
        }   
          
          
        // ------------------- Key
        case HostCmdType::Key: {
          const uint32_t host = (c.xid != 0) ? c.xid : ctx.input().focus_host;
          if (!host) break;

          // Clamp X11 keycode: mac_vk + 8, range [8..255]
          uint32_t kc32 = (uint32_t)c.keyCode + 8u;
          if (kc32 < 8u)   kc32 = 8u;
          if (kc32 > 255u) kc32 = 255u;
          const uint8_t x11_kc = (uint8_t)kc32;

          // Keep canonical mods in sync
          ctx.input().mods = c.modsMask;

          auto wantsKey = [&](uint32_t xid) -> bool {
            if (!xid) return false;
            const x11::WindowView* vw = ctx.window(xid);
            if (!vw || vw->owner_fd <= 0) return false;
            const uint32_t mask = vw->event_mask;
            const bool wantPress   = (mask & x11::mask::KeyPress) != 0;
            const bool wantRelease = (mask & x11::mask::KeyRelease) != 0;
            return c.isDown ? wantPress : wantRelease;
          };

          // Primary: keyboard focus (only if it belongs to this host)
          uint32_t target = 0;
          const uint32_t focus = ctx.input().focus_xid;
          if (focus != 0) {
            const uint32_t focusHost = ctx.windows().topLevelAncestorOf(focus);
            if (focusHost == host) target = focus;
          }
          if (!target) target = host; // fallback: host

          // If target doesn't select, climb parent chain until host (simple propagation).
          if (!wantsKey(target)) {
            uint32_t cur = target;
            int safety = 0;
            while (cur && cur != host) {
              x11::WindowView vw{};
              if (!ctx.windows().snapshot(cur, vw)) break;
              cur = vw.parent_xid;
              if (wantsKey(cur)) { target = cur; break; }
              if (++safety > 64) break;
            }
            if (!wantsKey(target) && wantsKey(host)) target = host;
            if (!wantsKey(target)) break; // nobody wants it
          }

        #ifndef NDEBUG
          fprintf(stderr,
                  "[KEY] host=0x%08X focus=0x%08X deliver=0x%08X down=%d kc=%u mods=0x%X\n",
                  (unsigned)host,
                  (unsigned)focus,
                  (unsigned)target,
                  (int)(c.isDown != 0),
                  (unsigned)x11_kc,
                  (unsigned)c.modsMask);
        #endif

          srv->eventOps().sendKeyEvent(ctx, target,
                                       c.isDown != 0,
                                       x11_kc,
                                       ctx.input().buttons, c.modsMask);
          break;
        }
          
      } // switch
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

// Legacy fallback for callers that only have DOWN/UP without a button number.
// If you don’t want this, you can omit it and just ignore DOWN/UP here.
extern "C" void x11_proto_bridge_post_pointer_button_legacy(uint32_t xid,
                                                int is_press,
                                                int32_t x_px, int32_t y_px,
                                                uint32_t buttons, uint32_t modifiers);


// pointer/mouse handling
extern "C" {

//void x11_proto_bridge_post_pointer_move(uint32_t xid,
//                                       int32_t x_px, int32_t y_px,
//                                       uint32_t buttons, uint32_t modifiers)
//{
//  
//  HostCmd c;
//  c.type = HostCmdType::PointerMove;
//  c.xid = xid;
//  c.x_px = x_px;
//  c.y_px = y_px;
//  c.buttonsMask = buttons;
//  c.modsMask = modifiers;
//  hostcmd_push(c);
//}

void x11_proto_bridge_post_pointer_button_legacy(uint32_t xid,
                                                int is_press,
                                                int32_t x_px, int32_t y_px,
                                                uint32_t buttons, uint32_t modifiers)
{
  HostCmd c;
  c.type = HostCmdType::Button;
  c.xid = xid;
  c.isDown = is_press ? 1 : 0;
  c.button = 0; // legacy / unknown button
  c.win_x_u = x_px;
  c.win_y_u = y_px;
  c.buttonsMask = buttons;
  c.modsMask = modifiers;
  hostcmd_push(c);
}

} // extern "C"

extern "C" void x11_proto_bridge_post_pointer_move2(uint32_t xid,
                                                   int32_t win_x_u, int32_t win_y_u,
                                                   int32_t root_x_u, int32_t root_y_u,
                                                   uint8_t deliver,
                                                   uint32_t buttons,
                                                   uint32_t modifiers)
{
  HostCmd c;
  c.type = HostCmdType::PointerMove;
  c.xid = xid;
  c.win_x_u = win_x_u;
  c.win_y_u = win_y_u;
  c.root_x_u = root_x_u;
  c.root_y_u = root_y_u;
  c.deliver = deliver;
  c.buttonsMask = buttons;
  c.modsMask = modifiers;
  hostcmd_push(c);
}


extern "C" void x11_proto_bridge_post_pointer_button(uint32_t xid,
                                                     uint8_t is_press,
                                                     uint8_t button,
                                                     int32_t win_x_u, int32_t win_y_u,
                                                     uint32_t buttons,
                                                     uint32_t modifiers)
{
  HostCmd c;
  c.type = HostCmdType::Button;
  c.xid = xid;
  c.isDown = is_press ? 1 : 0;
  c.button = button;
  c.win_x_u = win_x_u;
  c.win_y_u = win_y_u;
  c.buttonsMask = buttons;
  c.modsMask = modifiers;
  hostcmd_push(c);
}


extern "C" void x11_proto_bridge_post_scroll(uint32_t xid,
                                            uint8_t axis,
                                            int16_t ticks,
                                            int32_t win_x_u, int32_t win_y_u,
                                            uint32_t buttons,
                                            uint32_t modifiers)
{
  if (xid == 0) return;

  HostCmd c;
  c.type = HostCmdType::ScrollTicks;
  c.xid = xid;
  c.axis = axis;
  c.ticks = ticks;
  c.win_x_u = win_x_u;
  c.win_y_u = win_y_u;
  c.buttonsMask = buttons;
  c.modsMask = modifiers;
  hostcmd_push(c);
}

extern "C" void x11_proto_bridge_post_key(uint32_t xid,
                                         uint8_t is_down,
                                         uint32_t keycode,
                                         uint32_t modifiers)
{
  HostCmd c;
  c.type = HostCmdType::Key;
  c.xid = xid;               // may be 0 → route to focus on C++ side
  c.isDown = is_down ? 1 : 0;
  c.keyCode = keycode;
  c.modsMask = modifiers;
  hostcmd_push(c);
}

extern "C" void x11_proto_bridge_post_enter(uint32_t xid,
                                           int32_t win_x_u, int32_t win_y_u,
                                           uint32_t modifiers)
{
  if (xid == 0) return;

  HostCmd c;
  c.type = HostCmdType::PointerEnter;
  c.xid = xid;
  c.win_x_u = win_x_u;
  c.win_y_u = win_y_u;
  c.modsMask = modifiers;
  hostcmd_push(c);
}

extern "C" void x11_proto_bridge_post_leave(uint32_t xid,
                                           int32_t win_x_u, int32_t win_y_u,
                                           uint32_t modifiers)
{
  if (xid == 0) return;

  HostCmd c;
  c.type = HostCmdType::PointerLeave;
  c.xid = xid;
  c.win_x_u = win_x_u;
  c.win_y_u = win_y_u;
  c.modsMask = modifiers;
  hostcmd_push(c);
}

extern "C" void x11_proto_bridge_post_focus(uint32_t xid,
                                           uint8_t focused)
{
  if (xid == 0) return;

  HostCmd c;
  c.type = HostCmdType::Focus;
  c.xid = xid;
  c.focused = focused ? 1 : 0;
  hostcmd_push(c);
}


namespace {
  // Global pointer set once on the protocol/server thread.
  // Required so C can query window tree.
  const x11::WindowTable* g_windows = nullptr;
}

// Call this once after ctx is created (server/protocol thread).
extern "C" void x11_cpp_set_window_table(const x11::WindowTable* wt) {
  g_windows = wt;
}

extern "C" uint32_t x11_cpp_list_descendants(uint32_t host, uint32_t* out, uint32_t cap)
{
  if (!g_windows) return 0;
  if (!out || cap == 0) return 0;
  if (host == 0) return 0;

  std::vector<uint32_t> kids = g_windows->descendantsOf(host);

  const uint32_t n = (uint32_t)std::min<size_t>(kids.size(), cap);
  for (uint32_t i = 0; i < n; i++) out[i] = kids[i];
  return n;
}

extern "C" int x11_cpp_get_window_geom(uint32_t xid,
                                      uint32_t* out_parent,
                                      int16_t* out_x,
                                      int16_t* out_y,
                                      uint16_t* out_w,
                                      uint16_t* out_h,
                                      int* out_mapped)
{
  if (!g_windows) return 0;
  if (xid == 0) return 0;

  x11::WindowView vw{};
  if (!g_windows->snapshot(xid, vw)) return 0;

  if (out_parent) *out_parent = vw.parent_xid;
  if (out_x)      *out_x      = vw.x;
  if (out_y)      *out_y      = vw.y;
  if (out_w)      *out_w      = vw.w;
  if (out_h)      *out_h      = vw.h;
  if (out_mapped) *out_mapped = vw.mapped ? 1 : 0;

  return 1;
}

extern "C" int x11_cpp_get_abs_pos_in_host(uint32_t host, uint32_t xid,
                                          int32_t* out_abs_x,
                                          int32_t* out_abs_y)
{
  if (!g_windows) return 0;
  if (!out_abs_x || !out_abs_y) return 0;
  if (host == 0 || xid == 0) return 0;

  // Walk from xid up to host, accumulating relative x/y.
  int32_t ax = 0;
  int32_t ay = 0;
  uint32_t cur = xid;

  // Safety to avoid infinite loops if parent pointers get weird.
  for (int hop = 0; hop < 256; hop++) {
    x11::WindowView vw{};
    if (!g_windows->snapshot(cur, vw)) return 0;

    // Add this node’s offset in its parent.
    ax += (int32_t)vw.x;
    ay += (int32_t)vw.y;

    if (cur == host) {
      // We included host’s own x/y above; in typical X11, host x/y is relative to root.
      // For “position within host”, we should NOT include host’s offset.
      // So subtract host.x/host.y back out:
      ax -= (int32_t)vw.x;
      ay -= (int32_t)vw.y;

      *out_abs_x = ax;
      *out_abs_y = ay;
      return 1;
    }

    // Stop if we hit root-ish without reaching host.
    if (vw.parent_xid == 0 || vw.parent_xid == 1) return 0;

    cur = vw.parent_xid;
  }

  return 0;
}
