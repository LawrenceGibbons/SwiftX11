//
//  XProtoTransport.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <cstddef>
#include <cstdint>

#include "XProtoNotifyQueue.hpp"
#include "XProtoTransport.hpp"
#include "XProtoPendingNotify.hpp"
#include "EventOps.hpp"
#include "XProtoContext.hpp"

// Your existing dbg_require_xproto_thread is currently in C.
// Expose it as an extern so C++ can call it.
extern "C" void dbg_require_xproto_thread(const char* what);

namespace x11 {

XProtoTransport::XProtoTransport(XProtoContext& ctx, EventOps& evOps)
: ctx_(ctx), evOps_(evOps) {}

void XProtoTransport::attachClientFd(int fd) {
  client_fd_ = fd;
}

void XProtoTransport::setXprotoThreadSelf() {
  xproto_thread_ = pthread_self();
  xproto_thread_valid_ = true;
}

void XProtoTransport::noteLastSeq(uint16_t seq) {
  last_seq_ = seq;
}

uint16_t XProtoTransport::lastSeqOr1() const {
  return (last_seq_ == 0) ? 1 : last_seq_;
}

bool XProtoTransport::sendAll(const void* buf, std::size_t n) {
#ifndef NDEBUG
  dbg_require_xproto_thread("XProtoTransport::sendAll");
#endif
  if (!xproto_thread_valid_ || !pthread_equal(pthread_self(), xproto_thread_)) {
    ctx_.tracef("[XProtoTransport] sendAll called from non-xproto thread\n");
    return false;
  }
  if (client_fd_ < 0) return false;

  const uint8_t* p = static_cast<const uint8_t*>(buf);
  std::size_t left = n;

  while (left) {
    ssize_t w = ::send(client_fd_, p, left, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      return false;
    }
    if (w == 0) return false;
    p += static_cast<size_t>(w);
    left -= static_cast<size_t>(w);
  }
  return true;
}

bool XProtoTransport::sendReplyBytes(const void* buf, std::size_t n) {
  // For now this is identical to sendAll, but keeping a distinct API prevents
  // accidental use for events without ownership checks.
  return sendAll(buf, n);
}

  bool XProtoTransport::sendEvent32(uint32_t targetWid, const uint8_t ev[32]) {
    if (targetWid == 0 || !ev) {
      ctx_.tracef("[XProtoTransport] sendEvent32 DROP invalid args\n");
      return false;
    }
    if (!xproto_thread_valid_ || !pthread_equal(pthread_self(), xproto_thread_)) {
      ctx_.tracef("[XProtoTransport] sendEvent32 DROP wrong thread\n");
      return false;
    }
    if (client_fd_ < 0) {
      ctx_.tracef("[XProtoTransport] sendEvent32 DROP no client_fd\n");
      return false;
    }

    const WindowView* wv = ctx_.window(targetWid);
    if (!wv) {
      ctx_.tracef("[XProtoTransport] sendEvent32 DROP no window view wid=0x%08X\n", (unsigned)targetWid);
      return false;
    }

    if (wv->owner_fd <= 0 || wv->owner_fd != client_fd_) {
      ctx_.tracef("[XProtoTransport] sendEvent32 DROP owner mismatch wid=0x%08X owner_fd=%d client_fd=%d\n",
                  (unsigned)targetWid, wv->owner_fd, client_fd_);
      return false;
    }

    ctx_.tracef("[XProtoTransport] sendEvent32 OK wid=0x%08X type=%u\n",
                (unsigned)targetWid, (unsigned)ev[0]);
    return sendAll(ev, 32);
  }
  
  
void XProtoTransport::queueNotify(uint32_t wid, bool wantConfigure, bool wantExpose) {
  if (wid == 0) return;
  if (!wantConfigure && !wantExpose) return;
  ctx_.tracef("[XProtoTransport] queueNotify wid=0x%08X cfg=%d exp=%d\n",
              (unsigned)wid, (int)wantConfigure, (int)wantExpose);
  notifyQueue_.queueNotify(wid, wantConfigure, wantExpose);
}

void XProtoTransport::flushNotifyQueue() {
  
  // MUST be called only from the xproto thread.
  if (!xproto_thread_valid_) return;
  if (!pthread_equal(pthread_self(), xproto_thread_)) return;
  if (client_fd_ < 0) return;

  static constexpr std::size_t kMaxPending = 1024;
  PendingNotify local[kMaxPending];
  std::size_t n = notifyQueue_.drain(local, kMaxPending);

  const uint16_t seq0 = lastSeqOr1();

  if (n == 0) return;

#ifndef NDEBUG
  for (size_t i=0;i<n;i++){
    ctx_.tracef("  pn[%zu] wid=0x%08X cfg=%u exp=%u rect=%u\n",
                i, (unsigned)local[i].wid,
                (unsigned)local[i].want_configure,
                (unsigned)local[i].want_expose,
                (unsigned)local[i].expose_has_rect);
  }
#endif
  // Delegate the actual per-window filtering + event emission to EventOps.
  // EventOps is expected to consult XProtoContext for window state/event masks.
  for (size_t i = 0; i < n; i++) {
    evOps_.flushPendingNotify(local[i], seq0);
  }
}

void XProtoTransport::queueExposeRect(uint32_t wid,
                                      uint16_t x, uint16_t y,
                                      uint16_t w, uint16_t h,
                                      uint16_t count) {
  if (wid == 0) return;
  if (w == 0 || h == 0) return;
  notifyQueue_.queueExposeRect(wid, x, y, w, h, count);
}
  
} // namespace x11
