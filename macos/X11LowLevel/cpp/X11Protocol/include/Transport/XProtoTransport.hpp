//
//  XProtoTransport.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once
#include <cstdint>
#include <cstddef>
#include <pthread.h>

#include "XProtoNotifyQueue.hpp"
#include "XProtoPendingNotify.hpp"

namespace x11 {

class EventOps;
class XProtoContext;

class XProtoTransport {
public:
  XProtoTransport(XProtoContext& ctx, EventOps& evOps);

  // Call once per accepted client connection
  void attachClientFd(int fd);

  // Called by drain_requests when it knows it is on xproto thread
  void setXprotoThreadSelf();

  // Update “last seq seen” (same behavior as your g_last_seq)
  void noteLastSeq(uint16_t seq);

  // Queue coalesced notification for later flush on xproto thread
  void queueNotify(uint32_t wid, bool wantConfigure, bool wantExpose);

  // Drains and coalesces pending notifications, then delegates to EventOps for filtering/sending.
  void flushNotifyQueue();

  // Socket send primitive (enforces xproto thread)
  // Low-level primitive, do not use for events.
  bool sendAll(const void* buf, std::size_t n);

  // ---- API split (enforced policy) ----
  // Replies/handshake data: raw bytes to the current client connection.
  bool sendReplyBytes(const void* buf, std::size_t n);

  // Events: 32-byte X11 wire events routed to the window's owning client.
  // Enforces:
  //  - must be on xproto thread
  //  - window must exist
  //  - window owner_fd must match this transport's client_fd_
  // Returns false if any guard fails.
  bool sendEvent32(uint32_t targetWid, const uint8_t ev[32]);

  // Expose for EventOps
  int clientFd() const { return client_fd_; }
  uint16_t lastSeqOr1() const;

  void queueExposeRect(uint32_t wid,
                       uint16_t x, uint16_t y,
                       uint16_t w, uint16_t h,
                       uint16_t count);
  
private:
  XProtoContext& ctx_;
  EventOps& evOps_;

  int client_fd_ = -1;

  // Thread identity
  pthread_t xproto_thread_{};
  bool xproto_thread_valid_ = false;

  // Last request sequence
  uint16_t last_seq_ = 0;

  XProtoNotifyQueue notifyQueue_;

  // Internal helper: coalesce by wid
  void coalesceLocked(uint32_t wid, bool wantConfigure, bool wantExpose);
};

} // namespace x11
