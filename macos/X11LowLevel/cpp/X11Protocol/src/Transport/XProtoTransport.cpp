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

#include "Core/XProtoNotifyQueue.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Core/XProtoPendingNotify.hpp"
#include "Ops/EventOps.hpp"
#include "Core/XProtoContext.hpp"

// Your existing dbg_require_xproto_thread is currently in C.
// Expose it as an extern so C++ can call it.
extern "C" void dbg_require_xproto_thread(const char* what);

namespace x11 {

XProtoTransport::XProtoTransport(XProtoContext& ctx, EventOps& evOps)
  : ctx_(ctx), evOps_(evOps), last_seq_(0), event_seq_(0) {
    
#ifndef NDEBUG
    xproto_tid_ = pthread_self();
#endif
}

void XProtoTransport::attachClientFd(int fd) {
  client_fd_ = fd;
}

void XProtoTransport::setXprotoThreadSelf() {
  xproto_thread_ = pthread_self();
  xproto_thread_valid_ = true;
}

void XProtoTransport::noteLastSeq(uint16_t seq) {
  last_seq_ = seq;
  if (event_seq_ < seq) event_seq_ = seq;
}

uint16_t XProtoTransport::lastSeq() const {
  return last_seq_;
}

  
uint16_t XProtoTransport::nextEventSeq() {
  // Ensure nonzero (optional)
  if (event_seq_ == 0) event_seq_ = (last_seq_ ? last_seq_ : 1);
  // increment and return
  event_seq_ = (uint16_t)(event_seq_ + 1);
  if (event_seq_ == 0) event_seq_ = 1;
  return event_seq_;
}  
  
static inline void dbg_dump32(const char* tag, const uint8_t* b) {
  fprintf(stderr, "%s ", tag);
  for (int i = 0; i < 32; i++) fprintf(stderr, "%02X", b[i]);
  fprintf(stderr, "\n");
}

  
bool XProtoTransport::sendAll(const void* buf, std::size_t n) {
#ifndef NDEBUG
  const uint8_t* b = static_cast<const uint8_t*>(buf);

  auto dump8 = [&](const char* tag) {
    fprintf(stderr, "%s n=%zu head=", tag, n);
    for (size_t i = 0; i < 8 && i < n; i++) fprintf(stderr, "%02X", b[i]);
    fprintf(stderr, "\n");
  };

  // Must always be 4-byte aligned after setup (replies/events/payload chunks)
  if ((n % 4) != 0) {
    dump8("[SENDALL BADALIGN]");
  }

  if (n != 32) {
    dump8("[SENDALL CHUNK]");
  } else {
    // classify 32-byte packets
    const uint8_t b0 = b[0];
    const uint16_t seq = uint16_t(b[2] | (uint16_t(b[3]) << 8));
    if (b0 == 1) {
      const uint32_t lenw =
        uint32_t(b[4]) |
        (uint32_t(b[5]) << 8) |
        (uint32_t(b[6]) << 16) |
        (uint32_t(b[7]) << 24);
      fprintf(stderr, "[SENDALL REPLY32] seq=%u lenw=%u (%u bytes) rep1=%u\n",
              (unsigned)seq, (unsigned)lenw, (unsigned)(lenw * 4u), (unsigned)b[1]);
    } else if (b0 == 0) {
      fprintf(stderr, "[SENDALL ERROR32] seq=%u code=%u minor=%u major=%u\n",
              (unsigned)seq, (unsigned)b[1], (unsigned)b[10], (unsigned)b[11]);
    } else {
      fprintf(stderr, "[SENDALL EVENT32] type=%u detail=%u seqField=%u\n",
              (unsigned)b0, (unsigned)b[1], (unsigned)seq);
    }
  }
#endif
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
  
bool XProtoTransport::sendReplyBytes(const void* buf, std::size_t n)
{
  const uint8_t* b = static_cast<const uint8_t*>(buf);

#ifndef NDEBUG
  auto dumpHex = [&](const char* tag, const uint8_t* p, std::size_t nn) {
    fprintf(stderr, "%s ", tag);
    for (std::size_t i = 0; i < nn; i++) fprintf(stderr, "%02X", p[i]);
    fprintf(stderr, "\n");
  };

  // -----------------------------
  // Reply header (must be exactly 32 bytes)
  // -----------------------------
  if (n == 32) {
    const uint8_t r0   = b[0];
    const uint8_t rep1 = b[1];
    const uint16_t seq = (uint16_t)(b[2] | (uint16_t(b[3]) << 8));
    const uint32_t lenw =
      uint32_t(b[4]) |
      (uint32_t(b[5]) << 8) |
      (uint32_t(b[6]) << 16) |
      (uint32_t(b[7]) << 24);

    if (r0 != 1) {
      fprintf(stderr, "[PROTO BUG] sendReplyBytes header but r0=%u seq=%u\n",
              (unsigned)r0, (unsigned)seq);
      dumpHex("[PROTO BUG] hdr=", b, 32);
      // Don’t abort yet; return false so we fail fast without nuking everything.
      return false;
    }

    // If we still had an open reply, we under-sent payload previously.
    if (dbg_haveOpenReply_) {
      fprintf(stderr,
              "[PROTO BUG] New reply header before finishing prior payload: "
              "prev seq=%u sent=%u/%u bytes\n",
              (unsigned)dbg_openSeq_,
              (unsigned)dbg_openSentBytes_,
              (unsigned)dbg_openExpectBytes_);
      // Reset to recover so xeyes/xterm can proceed and you can see *next* bug.
      dbg_haveOpenReply_ = false;
    }

    dbg_openSeq_         = seq;
    dbg_openExpectBytes_ = lenw * 4u;
    dbg_openSentBytes_   = 0;
    dbg_haveOpenReply_   = true;

    fprintf(stderr,
            "[REPLYHDR] seq=%u lenw=%u (%u bytes) rep1=%u\n",
            (unsigned)seq, (unsigned)lenw, (unsigned)dbg_openExpectBytes_, (unsigned)rep1);

    if (seq == 3)  dumpHex("[REPLYHDRHEX seq=3]",  b, 32);
    if (seq == 10) dumpHex("[REPLYHDRHEX seq=10]", b, 32);

    if (dbg_openExpectBytes_ == 0) {
      fprintf(stderr, "[REPLYDONE] seq=%u payload=0\n", (unsigned)dbg_openSeq_);
      dbg_haveOpenReply_ = false;
    }

    return sendAll(buf, n);
  }

  // -----------------------------
  // Payload chunk (must follow a reply header)
  // -----------------------------
  if (!dbg_haveOpenReply_) {
    fprintf(stderr, "[PROTO BUG] Payload chunk n=%zu with no open reply\n", n);
    dumpHex("[PROTO BUG] payload8=", b, (n < 8 ? n : 8));
    // Don’t abort; just drop payload so we can keep going and collect more evidence.
    return true;
  }

  if (dbg_openSentBytes_ + (uint32_t)n > dbg_openExpectBytes_) {
    fprintf(stderr,
            "[PROTO BUG] Payload overflow for seq=%u: chunk=%zu makes %u/%u bytes\n",
            (unsigned)dbg_openSeq_, n,
            (unsigned)(dbg_openSentBytes_ + (uint32_t)n),
            (unsigned)dbg_openExpectBytes_);
    dumpHex("[PROTO BUG] overflow16=", b, (n < 16 ? n : 16));
    // Reset tracker to recover.
    dbg_haveOpenReply_ = false;
    return false;
  }

  dbg_openSentBytes_ += (uint32_t)n;

  fprintf(stderr,
          "[REPLYPAY] seq=%u chunk=%zu total=%u/%u\n",
          (unsigned)dbg_openSeq_, n,
          (unsigned)dbg_openSentBytes_, (unsigned)dbg_openExpectBytes_);

  if (dbg_openSentBytes_ == dbg_openExpectBytes_) {
    fprintf(stderr, "[REPLYDONE] seq=%u payload=%u\n",
            (unsigned)dbg_openSeq_, (unsigned)dbg_openExpectBytes_);
    dbg_haveOpenReply_ = false;
  }

  return sendAll(buf, n);

#else
  return sendAll(buf, n);
#endif
}

//bool XProtoTransport::sendReplyBytes(const void* buf, std::size_t n)
//{
//#ifndef NDEBUG
//  const uint8_t* in = static_cast<const uint8_t*>(buf);
//
//  // ---- stream state (DEBUG ONLY) ----
//  static bool     haveOpen   = false;
//  static uint16_t openSeq    = 0;
//  static uint32_t openExpect = 0;   // bytes expected after 32-byte header
//  static uint32_t openSent   = 0;   // bytes sent so far after header
//
//  static bool     haveHdrFrag = false;
//  static uint8_t  hdrBuf[32];
//  static uint32_t hdrHave = 0;
//
//  auto dumpHex = [&](const char* tag, const uint8_t* p, std::size_t nn) {
//    fprintf(stderr, "%s ", tag);
//    for (std::size_t i = 0; i < nn; i++) fprintf(stderr, "%02X", p[i]);
//    fprintf(stderr, "\n");
//  };
//
//  if (n == 32) {
//    
//    const uint16_t seq = (uint16_t)(in[2] | (uint16_t(in[3]) << 8));
//    if (seq == 53) {
//      fprintf(stderr, "[REPLYHDRHEX seq=53] ");
//      for (int i = 0; i < 32; i++) fprintf(stderr, "%02X", in[i]);
//      fprintf(stderr, "\n");
//    }
//  }
//  
//  auto parseAndLogHeader = [&](const uint8_t* h) {
//    const uint8_t  r0   = h[0];
//    const uint8_t  rep1 = h[1];
//    const uint16_t seq  = (uint16_t)(h[2] | (uint16_t(h[3]) << 8));
//    const uint32_t lenw =
//      uint32_t(h[4]) |
//      (uint32_t(h[5]) << 8) |
//      (uint32_t(h[6]) << 16) |
//      (uint32_t(h[7]) << 24);
//
//    if (r0 != 1) {
//      fprintf(stderr, "[PROTO BUG] Reply header but r0=%u\n", (unsigned)r0);
//      dumpHex("[PROTO BUG] hdr=", h, 32);
//      abort();
//    }
//
//    if (haveOpen) {
//      fprintf(stderr,
//              "[PROTO BUG] New reply header before finishing prior payload: "
//              "prev seq=%u sent=%u/%u\n",
//              (unsigned)openSeq, (unsigned)openSent, (unsigned)openExpect);
//      abort();
//    }
//    
//    if ( n == 32 ) {
//      // Sanity: a reply asking for an absurd payload is almost certainly corruption.
//      // Tune threshold if you want, but 1 MiB is already huge for core replies.
//      const uint64_t bytes = uint64_t(lenw) * 4ull;
//      if (bytes > (1ull << 20)) {
//        fprintf(stderr,
//                "[PROTO BUG] absurd reply length: seq=%u lenw=%u (%llu bytes)\n",
//                (unsigned)seq, (unsigned)lenw, (unsigned long long)bytes);
//        fprintf(stderr, "[PROTO BUG] hdr32 = ");
//        for (int i = 0; i < 32; i++) fprintf(stderr, "%02X", in[i]);
//        fprintf(stderr, "\n");
//        abort();
//      }
//    }
//    openSeq    = seq;
//    openExpect = lenw * 4u;
//    openSent   = 0;
//    haveOpen   = true;
//
//    fprintf(stderr,
//            "[REPLYHDR] seq=%u lenw=%u (%u bytes) rep1=%u\n",
//            (unsigned)seq, (unsigned)lenw, (unsigned)openExpect, (unsigned)rep1);
//
//    if (seq == 3)  dumpHex("[REPLYHDRHEX seq=3]",  h, 32);
//    if (seq == 10) dumpHex("[REPLYHDRHEX seq=10]", h, 32);
//
//    if (openExpect == 0) {
//      fprintf(stderr, "[REPLYDONE] seq=%u payload=0\n", (unsigned)openSeq);
//      haveOpen = false;
//    }
//  };
//
//  // ---- update debug state machine while forwarding bytes ----
//  std::size_t off = 0;
//  while (off < n) {
//
//    // If no reply open and not assembling header, next byte must be r0==1 (reply header).
//    if (!haveOpen && !haveHdrFrag) {
//      if (in[off] != 1) {
//        fprintf(stderr,
//                "[PROTO BUG] sendReplyBytes saw non-reply byte r0=%u while no reply open (n=%zu off=%zu)\n",
//                (unsigned)in[off], n, off);
//        dumpHex("[PROTO BUG] bytes16=", in + off, std::min<std::size_t>(16, n - off));
//        abort();
//      }
//      haveHdrFrag = true;
//      hdrHave = 0;
//    }
//
//    // Assemble header bytes (but DO NOT block sending — sending happens after the loop).
//    if (haveHdrFrag) {
//      const std::size_t want = 32 - hdrHave;
//      const std::size_t take = std::min(want, n - off);
//      memcpy(hdrBuf + hdrHave, in + off, take);
//      hdrHave += (uint32_t)take;
//      off += take;
//
//      if (hdrHave == 32) {
//        haveHdrFrag = false;
//        parseAndLogHeader(hdrBuf);
//      }
//      continue;
//    }
//
//    // Payload bytes for current reply:
//    if (haveOpen) {
//      const uint32_t remain = openExpect - openSent;
//      const std::size_t take = std::min<std::size_t>(remain, n - off);
//
//      openSent += (uint32_t)take;
//
//      fprintf(stderr,
//              "[REPLYPAY] seq=%u chunk=%zu total=%u/%u\n",
//              (unsigned)openSeq, take, (unsigned)openSent, (unsigned)openExpect);
//
//      off += take;
//
//      if (openSent == openExpect) {
//        fprintf(stderr, "[REPLYDONE] seq=%u payload=%u\n",
//                (unsigned)openSeq, (unsigned)openExpect);
//        haveOpen = false;
//      }
//      continue;
//    }
//
//    // If we’re here, we should be starting a new reply header but didn’t.
//    fprintf(stderr, "[PROTO BUG] state machine fell through\n");
//    abort();
//  }
//
//  // CRITICAL: actually forward the bytes we just analyzed.
//  return sendAll(buf, n);
//
//#else
//  return sendAll(buf, n);
//#endif
//}
  
// xxx old but seemed to work  bool XProtoTransport::sendReplyBytes(const void* buf, std::size_t n)
// xxx old but seemed to work{
// xxx old but seemed to work  const uint8_t* r = static_cast<const uint8_t*>(buf);
// xxx old but seemed to work
// xxx old but seemed to work#ifndef NDEBUG
// xxx old but seemed to work  static uint16_t last_reply_seq = 0xFFFF;
// xxx old but seemed to work#endif
// xxx old but seemed to work
// xxx old but seemed to work  // -----------------------------
// xxx old but seemed to work  // Reply header (must be 32 bytes)
// xxx old but seemed to work  // -----------------------------
// xxx old but seemed to work  if (n == 32) {
// xxx old but seemed to work    const uint8_t* r = (const uint8_t*)buf;
// xxx old but seemed to work    uint16_t seq = (uint16_t)(r[2] | (uint16_t(r[3])<<8));
// xxx old but seemed to work    if (seq == 3) {
// xxx old but seemed to work      fprintf(stderr, "[REPLYHDRHEX seq=3] ");
// xxx old but seemed to work      for (int i=0;i<32;i++) fprintf(stderr, "%02X", r[i]);
// xxx old but seemed to work      fprintf(stderr, "\n");
// xxx old but seemed to work    }
// xxx old but seemed to work  }
// xxx old but seemed to work  if (n >= 32) {
// xxx old but seemed to work    const uint8_t* r = (const uint8_t*)buf;
// xxx old but seemed to work    if (r[0] == 1) {
// xxx old but seemed to work      uint16_t seq = (uint16_t)(r[2] | (uint16_t(r[3]) << 8));
// xxx old but seemed to work      uint32_t lenw = (uint32_t)(r[4] |
// xxx old but seemed to work                                (uint32_t(r[5]) << 8) |
// xxx old but seemed to work                                (uint32_t(r[6]) << 16) |
// xxx old but seemed to work                                (uint32_t(r[7]) << 24));
// xxx old but seemed to work      fprintf(stderr, "[REPLYHDR] n=%zu seq=%u lenw=%u (%u bytes) rep1=%u\n",
// xxx old but seemed to work              n, (unsigned)seq, (unsigned)lenw, (unsigned)(lenw*4u), (unsigned)r[1]);
// xxx old but seemed to work
// xxx old but seemed to work      if (seq == 10) {
// xxx old but seemed to work        fprintf(stderr, "[REPLYHDRHEX seq=10] ");
// xxx old but seemed to work        for (int i = 0; i < 32; i++) fprintf(stderr, "%02X", r[i]);
// xxx old but seemed to work        fprintf(stderr, "\n");
// xxx old but seemed to work      }
// xxx old but seemed to work    }
// xxx old but seemed to work  }
// xxx old but seemed to work  if (n == 32) {
// xxx old but seemed to work    const uint8_t  r0   = r[0];
// xxx old but seemed to work    const uint8_t  rep1 = r[1];
// xxx old but seemed to work    const uint16_t seq  = uint16_t(r[2] | (uint16_t(r[3]) << 8));
// xxx old but seemed to work    const uint32_t lenw =
// xxx old but seemed to work      uint32_t(r[4]) |
// xxx old but seemed to work      (uint32_t(r[5]) << 8) |
// xxx old but seemed to work      (uint32_t(r[6]) << 16) |
// xxx old but seemed to work      (uint32_t(r[7]) << 24);
// xxx old but seemed to work
// xxx old but seemed to work    // Invariant 1: reply header must have r0 == 1
// xxx old but seemed to work    if (r0 != 1) {
// xxx old but seemed to work      fprintf(stderr,
// xxx old but seemed to work              "[PROTO BUG] sendReplyBytes called with 32-byte packet but r0=%u (seq=%u)\n",
// xxx old but seemed to work              (unsigned)r0, (unsigned)seq);
// xxx old but seemed to work      fprintf(stderr, "[PROTO BUG] header bytes: ");
// xxx old but seemed to work      for (int i = 0; i < 32; i++) fprintf(stderr, "%02X", r[i]);
// xxx old but seemed to work      fprintf(stderr, "\n");
// xxx old but seemed to work      abort();
// xxx old but seemed to work    }
// xxx old but seemed to work
// xxx old but seemed to work#ifndef NDEBUG
// xxx old but seemed to work    // Invariant 2: exactly one reply per sequence
// xxx old but seemed to work    if (seq == last_reply_seq) {
// xxx old but seemed to work      fprintf(stderr,
// xxx old but seemed to work              "[PROTO BUG] duplicate reply header for seq=%u\n",
// xxx old but seemed to work              (unsigned)seq);
// xxx old but seemed to work      abort();
// xxx old but seemed to work    }
// xxx old but seemed to work    last_reply_seq = seq;
// xxx old but seemed to work#endif
// xxx old but seemed to work
// xxx old but seemed to work    fprintf(stderr,
// xxx old but seemed to work            "[REPLY] seq=%u lenw=%u (%u bytes) rep1=%u\n",
// xxx old but seemed to work            (unsigned)seq,
// xxx old but seemed to work            (unsigned)lenw,
// xxx old but seemed to work            (unsigned)(lenw * 4u),
// xxx old but seemed to work            (unsigned)rep1);
// xxx old but seemed to work
// xxx old but seemed to work    return sendAll(buf, n);
// xxx old but seemed to work  }
// xxx old but seemed to work
// xxx old but seemed to work  // -----------------------------
// xxx old but seemed to work  // Payload (must follow a reply)
// xxx old but seemed to work  // -----------------------------
// xxx old but seemed to work  fprintf(stderr, "[REPLY_PAYLOAD] n=%zu\n", n);
// xxx old but seemed to work  return sendAll(buf, n);
// xxx old but seemed to work}
  
  
bool XProtoTransport::sendEvent32(uint32_t targetWid, const uint8_t ev[32]) {
    
#ifndef NDEBUG
  fprintf(stderr,
          "[EVENT] type=%u wid=0x%08X seq=%u bytes0-8=%02X%02X%02X%02X%02X%02X%02X%02X\n",
          (unsigned)ev[0],
          (unsigned)targetWid,
          (unsigned)lastSeq(),
          ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
  if (ev[0] == 0) {
    fprintf(stderr, "[FATAL] sending event type 0 (invalid)\n");
    abort();
  }
#endif
    
    
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

  const uint16_t seq0 = lastSeq();

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
