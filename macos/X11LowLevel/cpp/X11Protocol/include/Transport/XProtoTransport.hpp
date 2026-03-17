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

#include <Core/XProtoNotifyQueue.hpp>
#include <Core/XProtoPendingNotify.hpp>

namespace x11 {

class EventOps;
class XProtoContext;

struct ClientIdSpace {
  uint32_t base = 0;
  uint32_t mask = 0;

  bool valid() const { return mask != 0; }
  bool contains(uint32_t xid) const { return (xid & ~mask) == base; }
};


class XProtoTransport {
public:
  XProtoTransport(XProtoContext& ctx, EventOps& evOps);

  // Call once per accepted client connection
  void attachClientFd(int fd);

  // Called by drain_requests when it knows it is on xproto thread
  void setXprotoThreadSelf();

  // Update “last seq seen” 
  uint16_t nextEventSeq();
  void noteLastSeq(uint16_t seq);

  // Queue coalesced notification for later flush on xproto thread
  void queueNotify(uint32_t wid, bool wantConfigure, bool wantExpose);

  // Drains and coalesces pending notifications, then delegates to EventOps for filtering/sending.
  void flushNotifyQueue();

  // Socket send primitive (enforces xproto thread)
  // Low-level primitive, do not use for events.
  bool sendAll(const void* buf, std::size_t n);
  
  // manage cliend ID's for multi-client work
  void setClientIdSpace(uint32_t base, uint32_t mask) { idspace_.base = base; idspace_.mask = mask; }
  const ClientIdSpace& clientIdSpace() const { return idspace_; }
  bool clientOwnsXid(uint32_t xid) const { return idspace_.valid() && idspace_.contains(xid); }


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
  bool sendEventVariable(uint32_t targetWid, const uint8_t* ev, size_t len);
  bool sendMapNotify(uint16_t seq, uint32_t event, uint32_t window, bool overrideRedirect);
  bool sendUnmapNotify(uint16_t seq, uint32_t event, uint32_t window, bool fromConfigure);

  // Expose for EventOps
  int clientFd() const { return client_fd_; }
  uint16_t lastSeq() const;

  void queueExposeRect(uint32_t wid,
                       uint16_t x, uint16_t y,
                       uint16_t w, uint16_t h,
                       uint16_t count);
  
  //Accessor(s)
  XProtoNotifyQueue& notifyQueue() { return notifyQueue_; }

  // remember the last event we handled
  uint8_t last_request_major_ = 0;
  uint8_t last_request_minor_ = 0;
  uint16_t last_request_seq_  = 0;
  
  // handle error replies
  bool sendError32(const uint8_t e[32]); // raw
  bool sendErrorCore(uint8_t errorCode, uint16_t seq, uint32_t resourceId, uint8_t majorCode);

  void debugResetReplyTracker(); // call on begin/end session

  // Reply-sent tracking for sequence desync safety net.
  // Reset before each dispatch; set when a reply or error is sent.
  void resetReplySent() { reply_sent_ = false; }
  bool wasReplySent() const { return reply_sent_; }

private:
  
  int client_fd_ = -1;
  ClientIdSpace idspace_{};

  XProtoContext& ctx_;
  EventOps& evOps_;

  // Thread identity
  pthread_t xproto_thread_{};
  bool xproto_thread_valid_ = false;

  // Last request sequence
  uint16_t last_seq_  = 0;
  uint16_t event_seq_ = 0;

  // Monotonic wire-sequence floor.  XCB requires that response sequences
  // (events, replies, errors) never go backwards.  This tracks the highest
  // sequence ever sent on the wire so sendAll() can bump stale sequences.
  uint16_t max_wire_seq_ = 0;

  // Reply-payload tracking.  After a reply header (b0==1) with lenw > 0,
  // this counts remaining payload bytes.  sendAll() skips the monotonic
  // floor for payload data (bytes[2:3] are arbitrary payload, not sequences).
  uint32_t payload_remaining_ = 0;

  // True if a reply (byte[0]==1) or error (byte[0]==0) was sent since resetReplySent().
  bool reply_sent_ = false;
  
  XProtoNotifyQueue notifyQueue_;

  // Internal helper: coalesce by wid
  void coalesceLocked(uint32_t wid, bool wantConfigure, bool wantExpose);
  
#ifndef NDEBUG
  pthread_t xproto_tid_{};
  bool     dbg_haveOpenReply_   = false;
  uint16_t dbg_openSeq_         = 0;
  uint32_t dbg_openExpectBytes_ = 0; // bytes expected after 32-byte header
  uint32_t dbg_openSentBytes_   = 0; // bytes sent so far after header
#endif
  
}; // class

} // namespace x11
