//
//  XProtoNotifyBridge.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/20/26.
//

#include "XProtoNotifyQueue.hpp"
#include "EventOps.hpp"
#include "XProtoContext.hpp"   // whatever you currently have

#include <atomic>

using namespace x11;

// A single bridge instance is enough for bring-up (one client connection at a time).
static std::atomic<XProtoNotifyQueue*> g_q{nullptr};
static std::atomic<EventOps*>          g_ev{nullptr};
static std::atomic<XProtoContext*>     g_ctx{nullptr};

extern "C" void x11_cpp_notify_init(void* ctx_ptr, void* event_ops_ptr, void* queue_ptr) {
  g_ctx.store(reinterpret_cast<XProtoContext*>(ctx_ptr), std::memory_order_release);
  g_ev.store(reinterpret_cast<EventOps*>(event_ops_ptr), std::memory_order_release);
  g_q.store(reinterpret_cast<XProtoNotifyQueue*>(queue_ptr), std::memory_order_release);
}

extern "C" void x11_cpp_notify_queue(uint32_t wid, int want_cfg, int want_exp) {
  auto* q = g_q.load(std::memory_order_acquire);
  if (!q) return;
  q->queueNotify(wid, want_cfg != 0, want_exp != 0);
}

extern "C" size_t x11_cpp_notify_drain(void* out_array, size_t max_items) {
  auto* q = g_q.load(std::memory_order_acquire);
  if (!q) return 0;
  return q->drain(reinterpret_cast<PendingNotify*>(out_array), max_items);
}

extern "C" void x11_cpp_notify_flush_one(const void* pending_notify, uint16_t seq) {
  auto* ev = g_ev.load(std::memory_order_acquire);
  if (!ev) return;
  const auto& pn = *reinterpret_cast<const PendingNotify*>(pending_notify);
  ev->flushPendingNotify(pn, seq);
}
