//
//  XProtoTransport.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>
#include <errno.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Core/XProtoNotifyQueue.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Core/XProtoPendingNotify.hpp"
#include "Ops/EventOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Utils/WireLE.hpp" // your wire::wr16_le / wr32_le etc
#include "Utils/WireErrors.hpp"
#include "Utils/TraceDefs.hpp"

// Stub: the old C-level xproto thread check was in the deleted x11_xproto.c.
// XProtoTransport::sendAll() has its own thread check (xproto_thread_valid_),
// so this is now a no-op.
static inline void dbg_require_xproto_thread(const char* /*what*/) {}

namespace x11 {

static constexpr uint8_t kUnmapNotify = 18;
static constexpr uint8_t kMapNotify   = 19;


XProtoTransport::XProtoTransport(XProtoContext& ctx, EventOps& evOps)
  : ctx_(ctx), evOps_(evOps), last_seq_(0), event_seq_(0) {
    
#ifndef NDEBUG
    xproto_tid_ = pthread_self();
#endif
}

void XProtoTransport::attachClientFd(int fd) {
  client_fd_ = fd;
  max_wire_seq_ = 0;       // reset monotonic floor for new connection
  payload_remaining_ = 0;  // reset payload tracking
  dbg_last_sent_seq_ = 0;  // reset sequence regression detector
  dbg_send_count_ = 0;
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
  
#ifdef X11_TRACE_VERBOSE
static inline void dbg_dump32(const char* tag, const uint8_t* b) {
  fprintf(stderr, "%s ", tag);
  for (int i = 0; i < 32; i++) fprintf(stderr, "%02X", b[i]);
  fprintf(stderr, "\n");
}
#endif

bool XProtoTransport::sendAll(const void* buf, std::size_t n) {
#ifdef X11_TRACE_VERBOSE
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

  // Sequence-regression detector: catch when a reply/error/event has a
  // sequence that goes BACKWARDS compared to the last sent response.
  // This is the root cause of XCB "Unknown sequence number" crashes.
  if (n == 32) {
    const uint8_t* bb = static_cast<const uint8_t*>(buf);
    const uint8_t b0 = bb[0];
    const uint16_t seq = uint16_t(bb[2] | (uint16_t(bb[3]) << 8));

    // Track per-transport state for regression detection.
    // (Must NOT be thread_local — that leaks across client connections.)
    dbg_send_count_++;

    const char* kind = (b0 == 0) ? "ERROR" : (b0 == 1) ? "REPLY" : "EVENT";

    // Detect regression: a reply or error with seq < previous reply/error seq.
    // Events are allowed to have older sequences, but replies/errors must not
    // go backwards relative to each other (that causes XCB desync).
    if ((b0 == 0 || b0 == 1) && dbg_last_sent_seq_ != 0) {
      // Check if this reply/error seq is LESS than the last one sent
      int16_t delta = (int16_t)(seq - dbg_last_sent_seq_);
      if (delta < 0) {
        fprintf(stderr,
                "[SEQ_REGRESS] *** SEQUENCE WENT BACKWARDS *** "
                "%s seq=%u < prev_seq=%u (delta=%d) type=%u send#=%u\n",
                kind, (unsigned)seq, (unsigned)dbg_last_sent_seq_,
                (int)delta, (unsigned)b0, dbg_send_count_);
      }
    }

    // Update tracking for replies and errors only (they advance XCB's request_read).
    if (b0 == 0 || b0 == 1) {
      dbg_last_sent_seq_ = seq;
    }

    // Log the wire send (compact format for debugging).
    // Gated by X11_TRACE_WIRE (or X11_TRACE_VERBOSE) — very noisy.
#if X11_TRACE_WIRE_ENABLED
    if (b0 == 0) {
      fprintf(stderr, "[WIRE] #%u ERROR seq=%u code=%u major=%u\n",
              send_count, (unsigned)seq, (unsigned)bb[1], (unsigned)bb[11]);
    } else if (b0 == 1) {
      const uint32_t lenw =
        uint32_t(bb[4]) | (uint32_t(bb[5])<<8) |
        (uint32_t(bb[6])<<16) | (uint32_t(bb[7])<<24);
      fprintf(stderr, "[WIRE] #%u REPLY seq=%u lenw=%u rep1=%u\n",
              send_count, (unsigned)seq, (unsigned)lenw, (unsigned)bb[1]);
    } else {
      fprintf(stderr, "[WIRE] #%u EVENT type=%u seq=%u\n",
              send_count, (unsigned)b0, (unsigned)seq);
    }
#endif
  }
#endif
  if (!xproto_thread_valid_ || !pthread_equal(pthread_self(), xproto_thread_)) {
    ctx_.tracef("[XProtoTransport] sendAll called from non-xproto thread\n");
    return false;
  }
  if (client_fd_ < 0) return false;

  // Track reply/error sends: byte[0]==1 (Reply) or byte[0]==0 (Error)
  // for the sequence-desync safety net in XProtoServer::dispatch().
  // Only check when not in payload (payload bytes are arbitrary).
  if (n >= 32 && payload_remaining_ == 0) {
    const uint8_t b0 = static_cast<const uint8_t*>(buf)[0];
    if (b0 == 0 || b0 == 1) reply_sent_ = true;
  }

  // ── Monotonic wire-sequence floor ──────────────────────────────
  // XCB widens 16-bit response sequences to 64-bit and requires they
  // never go backwards across ANY response type (events included).
  // When drainHostCommands() interleaves with readAndDispatch(), events
  // can carry stale (older) sequences.  Fix: if bytes[2:3] of any
  // response HEADER would regress below the highest previously sent,
  // bump it up to that floor.
  //
  // CRITICAL: reply payload chunks also flow through sendAll().  Their
  // bytes[2:3] are arbitrary data, NOT sequence numbers.  We must NOT
  // read or modify those bytes.  payload_remaining_ tracks how many
  // payload bytes are still expected after a reply header.
  const void* sendBuf = buf;
  uint8_t fixedPkt[32];

  if (payload_remaining_ > 0) {
    // We are in the middle of reply payload — skip floor logic entirely.
    uint32_t consumed = (n < static_cast<std::size_t>(payload_remaining_))
                          ? static_cast<uint32_t>(n)
                          : payload_remaining_;
    payload_remaining_ -= consumed;
  } else if (n >= 32 && last_request_seq_ != 0) {
    // This is a new response header (event/reply/error), past setup phase.
    const uint8_t* bp = static_cast<const uint8_t*>(buf);
    const uint8_t b0 = bp[0];
    uint16_t pkt_seq = uint16_t(bp[2] | (uint16_t(bp[3]) << 8));

    // Apply monotonic floor
    if (max_wire_seq_ != 0) {
      int16_t delta = (int16_t)(pkt_seq - max_wire_seq_);
      if (delta < 0) {
        // Sequence would regress — bump to monotonic floor
#ifndef NDEBUG
        fprintf(stderr, "[SEQ_FLOOR] bumped seq %u → %u (type=%u)\n",
                (unsigned)pkt_seq, (unsigned)max_wire_seq_, (unsigned)bp[0]);
#endif
        std::memcpy(fixedPkt, bp, 32);
        wire::wr16_le(fixedPkt + 2, max_wire_seq_);
        pkt_seq = max_wire_seq_;

        if (n == 32) {
          sendBuf = fixedPkt;
        } else {
          // Combined header+payload: send fixed header, then original tail.
          const uint8_t* hp = fixedPkt;
          std::size_t hleft = 32;
          while (hleft) {
            ssize_t w = ::send(client_fd_, hp, hleft, MSG_NOSIGNAL);
            if (w < 0) {
              if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
              return false;
            }
            if (w == 0) return false;
            hp += static_cast<std::size_t>(w);
            hleft -= static_cast<std::size_t>(w);
          }
          const uint8_t* pp = static_cast<const uint8_t*>(buf) + 32;
          std::size_t pleft = n - 32;
          while (pleft) {
            ssize_t w = ::send(client_fd_, pp, pleft, MSG_NOSIGNAL);
            if (w < 0) {
              if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
              return false;
            }
            if (w == 0) return false;
            pp += static_cast<std::size_t>(w);
            pleft -= static_cast<std::size_t>(w);
          }
          // Track payload for this combined reply
          if (b0 == 1) {
            uint32_t lenw = uint32_t(bp[4]) | (uint32_t(bp[5])<<8) |
                            (uint32_t(bp[6])<<16) | (uint32_t(bp[7])<<24);
            uint32_t total = lenw * 4u;
            uint32_t included = static_cast<uint32_t>(n - 32);
            payload_remaining_ = (total > included) ? (total - included) : 0;
          }
          max_wire_seq_ = pkt_seq;
          return true;
        }
      }
    }
    max_wire_seq_ = pkt_seq;

    // If this is a reply header with payload, track remaining bytes.
    if (b0 == 1) {
      uint32_t lenw = uint32_t(bp[4]) | (uint32_t(bp[5])<<8) |
                      (uint32_t(bp[6])<<16) | (uint32_t(bp[7])<<24);
      uint32_t total = lenw * 4u;
      uint32_t included = (n > 32) ? static_cast<uint32_t>(n - 32) : 0;
      payload_remaining_ = (total > included) ? (total - included) : 0;
    }
  }

  const uint8_t* p = static_cast<const uint8_t*>(sendBuf);
  std::size_t left = n;

  int eagain_waits = 0;
  while (left) {
    ssize_t w = ::send(client_fd_, p, left, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Socket buffer full — wait with poll() instead of tight-spinning.
        // We MUST NOT drop data: it could be part of a reply the client is
        // waiting for.  The 1MB SO_SNDBUF set at accept time makes this
        // very rare.  poll() yields the CPU and wakes when the client reads.
        if (++eagain_waits == 1) {
          // First EAGAIN — log to help diagnose backpressure issues
          const uint8_t* hdr = static_cast<const uint8_t*>(sendBuf);
          fprintf(stderr, "[BACKPRESSURE] sendAll EAGAIN fd=%d n=%zu left=%zu type=%u\n",
                  client_fd_, n, left, (unsigned)hdr[0]);
        }
        struct pollfd pfd = { client_fd_, POLLOUT, 0 };
        ::poll(&pfd, 1, 50);
        continue;
      }
      return false;
    }
    if (w == 0) return false;
    p += static_cast<size_t>(w);
    left -= static_cast<size_t>(w);
  }
  if (eagain_waits > 0) {
    fprintf(stderr, "[BACKPRESSURE] resolved after %d waits (%zu bytes)\n",
            eagain_waits, n);
  }
  return true;
}
  
  bool XProtoTransport::sendReplyBytes(const void* buf, std::size_t n)
  {
    if (!buf || n == 0) return true;

  #ifdef X11_TRACE_VERBOSE
    const uint8_t* b = static_cast<const uint8_t*>(buf);

    auto dumpHex = [&](const char* tag, const uint8_t* p, std::size_t nn) {
      fprintf(stderr, "%s ", tag);
      for (std::size_t i = 0; i < nn; i++) fprintf(stderr, "%02X", p[i]);
      fprintf(stderr, "\n");
    };

  auto rd16le = [&](const uint8_t* p) -> uint16_t {
    return (uint16_t)(p[0] | (uint16_t(p[1]) << 8));
  };
  auto rd32le = [&](const uint8_t* p) -> uint32_t {
    return (uint32_t)(p[0] |
                      (uint32_t(p[1]) << 8) |
                      (uint32_t(p[2]) << 16) |
                      (uint32_t(p[3]) << 24));
  };

  // Helpers to decide if this buffer is "definitely a reply for the last request"
  auto looksLikeReplyForLastSeq = [&](const uint8_t* p, std::size_t nn) -> bool {
    if (nn < 8) return false;
    if (p[0] != 1) return false;                 // Reply
    if (last_request_seq_ == 0) return false;    // handshake phase or unknown
    const uint16_t seq = rd16le(p + 2);
    return (seq == last_request_seq_);
  };

  // -----------------------------
  // Combined reply: header + payload in one sendReplyBytes() call.
  // Only accept if:
  //  - it is a reply for last_request_seq_
  //  - total size == 32 + lengthWords*4 exactly
  // -----------------------------
  if (n > 32 && looksLikeReplyForLastSeq(b, n)) {
    const uint16_t seq  = rd16le(b + 2);
    const uint32_t lenw = rd32le(b + 4);

    const uint32_t expectPayload = lenw * 4u;
    const uint32_t havePayload   = (uint32_t)(n - 32);

    if (havePayload != expectPayload) {
      fprintf(stderr,
              "[PROTO BUG] Combined reply size mismatch seq=%u lenw=%u: n=%zu implies payload=%u but expect=%u\n",
              (unsigned)seq, (unsigned)lenw, n,
              (unsigned)havePayload, (unsigned)expectPayload);
      dumpHex("[PROTO BUG] combined16=", b, (n < 16 ? n : 16));
      return false;
    }

    if (dbg_haveOpenReply_) {
      fprintf(stderr,
              "[PROTO BUG] Combined reply begins before finishing prior payload: "
              "prev seq=%u sent=%u/%u bytes\n",
              (unsigned)dbg_openSeq_,
              (unsigned)dbg_openSentBytes_,
              (unsigned)dbg_openExpectBytes_);
      dbg_haveOpenReply_ = false; // recover
    }

    fprintf(stderr,
            "[REPLYHDR] (combined) seq=%u lenw=%u (%u bytes)\n",
            (unsigned)seq, (unsigned)lenw, (unsigned)expectPayload);
    fprintf(stderr, "[REPLYDONE] seq=%u payload=%u\n",
            (unsigned)seq, (unsigned)expectPayload);

    // combined send completes immediately
    dbg_haveOpenReply_   = false;
    dbg_openSeq_         = 0;
    dbg_openExpectBytes_ = 0;
    dbg_openSentBytes_   = 0;

    return sendAll(buf, n);
  }

  // -----------------------------
  // 32-byte reply header
  // Only treat it as a reply if it matches last_request_seq_ (avoids handshake confusion).
  // -----------------------------
  if (n == 32 && looksLikeReplyForLastSeq(b, n)) {
    const uint8_t  r0   = b[0];
    const uint8_t  rep1 = b[1];
    const uint16_t seq  = rd16le(b + 2);
    const uint32_t lenw = rd32le(b + 4);

    if (r0 != 1) {
      fprintf(stderr, "[PROTO BUG] sendReplyBytes header but r0=%u seq=%u\n",
              (unsigned)r0, (unsigned)seq);
      dumpHex("[PROTO BUG] hdr=", b, 32);
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
      dbg_haveOpenReply_ = false; // recover
    }

    dbg_openSeq_         = seq;
    dbg_openExpectBytes_ = lenw * 4u;
    dbg_openSentBytes_   = 0;
    dbg_haveOpenReply_   = true;

    fprintf(stderr,
            "[REPLYHDR] seq=%u lenw=%u (%u bytes) rep1=%u\n",
            (unsigned)seq, (unsigned)lenw, (unsigned)dbg_openExpectBytes_, (unsigned)rep1);

    if (dbg_openExpectBytes_ == 0) {
      fprintf(stderr, "[REPLYDONE] seq=%u payload=0\n", (unsigned)dbg_openSeq_);
      dbg_haveOpenReply_ = false;
    }

    return sendAll(buf, n);
  }

  // -----------------------------
  // Payload chunk (must follow a tracked reply header)
  // -----------------------------
  if (dbg_haveOpenReply_) {
    if (dbg_openSentBytes_ + (uint32_t)n > dbg_openExpectBytes_) {
      fprintf(stderr,
              "[PROTO BUG] Payload overflow for seq=%u: chunk=%zu makes %u/%u bytes\n",
              (unsigned)dbg_openSeq_, n,
              (unsigned)(dbg_openSentBytes_ + (uint32_t)n),
              (unsigned)dbg_openExpectBytes_);
      dumpHex("[PROTO BUG] overflow16=", b, (n < 16 ? n : 16));
      dbg_haveOpenReply_ = false; // recover
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
  }

  // -----------------------------
  // Raw/untracked send (handshake, setup, etc.)
  // If we're mid-session (last_request_seq_ != 0) and you hit this a lot,
  // that's usually a bug — but don't brick bring-up.
  // -----------------------------
  if (last_request_seq_ != 0 && n != 32) {
    fprintf(stderr,
            "[WARN] Untracked sendReplyBytes n=%zu (last_seq=%u). Treating as raw.\n",
            n, (unsigned)last_request_seq_);
    dumpHex("[WARN] raw16=", b, (n < 16 ? n : 16));
  }

  return sendAll(buf, n);

#else
  return sendAll(buf, n);
#endif
}
  
  
bool XProtoTransport::sendEvent32(uint32_t targetWid, const uint8_t ev[32]) {

#ifdef X11_TRACE_VERBOSE
  const uint8_t type = ev[0];
  if (type == 22) { // ConfigureNotify
    auto rd16 = [&](int off) -> uint16_t {
      return (uint16_t)(ev[off] | (uint16_t(ev[off+1]) << 8));
    };
    auto rd32 = [&](int off) -> uint32_t {
      return (uint32_t)(ev[off] |
                        (uint32_t(ev[off+1]) << 8) |
                        (uint32_t(ev[off+2]) << 16) |
                        (uint32_t(ev[off+3]) << 24));
    };

    const uint32_t eventWin  = rd32(4);
    const uint32_t windowWin = rd32(8);
    const int16_t x = (int16_t)rd16(16);
    const int16_t y = (int16_t)rd16(18);
    const uint16_t w = rd16(20);
    const uint16_t h = rd16(22);

    fprintf(stderr,
            "[CFG_EVT] target=0x%08X event=0x%08X window=0x%08X xy=%d,%d wh=%u,%u\n",
            (unsigned)targetWid, (unsigned)eventWin, (unsigned)windowWin,
            (int)x, (int)y, (unsigned)w, (unsigned)h);
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
#ifndef NDEBUG
    fprintf(stderr, "[EVENT_DROP] sendEvent32 owner mismatch wid=0x%08X type=%u owner_fd=%d client_fd=%d\n",
            (unsigned)targetWid, (unsigned)ev[0], wv->owner_fd, client_fd_);
#endif
    ctx_.tracef("[XProtoTransport] sendEvent32 DROP owner mismatch wid=0x%08X owner_fd=%d client_fd=%d\n",
                (unsigned)targetWid, wv->owner_fd, client_fd_);
    return false;
  }

  ctx_.tracef("[XProtoTransport] sendEvent32 OK wid=0x%08X type=%u\n",
              (unsigned)targetWid, (unsigned)ev[0]);
  return sendAll(ev, 32);
}

bool XProtoTransport::sendEventVariable(uint32_t targetWid, const uint8_t* ev, size_t len) {
  if (targetWid == 0 || !ev || len < 32) return false;
  if (!xproto_thread_valid_ || !pthread_equal(pthread_self(), xproto_thread_)) return false;
  if (client_fd_ < 0) return false;

  const WindowView* wv = ctx_.window(targetWid);
  if (!wv) return false;
  if (wv->owner_fd <= 0 || wv->owner_fd != client_fd_) return false;

  return sendAll(ev, len);
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
  

inline void sendMapNotify(XProtoContext& ctx,
                          uint16_t seq,
                          uint32_t event,
                          uint32_t window,
                          bool overrideRedirect)
{
  std::array<uint8_t,32> ev{};
  ev.fill(0);

  ev[0] = kMapNotify;
  ev[1] = 0; // unused for MapNotify

  wire::wr16_le(ev.data() + 2, seq);
  wire::wr32_le(ev.data() + 4, event);
  wire::wr32_le(ev.data() + 8, window);
  ev[12] = overrideRedirect ? 1 : 0;

  (void)ctx.transport().sendEvent32(window, ev.data());
}
  
  
inline void sendUnmapNotify(XProtoContext& ctx,
                            uint16_t seq,
                            uint32_t event,
                            uint32_t window,
                            bool fromConfigure)
{
  std::array<uint8_t,32> ev{};
  ev.fill(0);

  ev[0] = kUnmapNotify;
  ev[1] = fromConfigure ? 1 : 0; // fromConfigure lives in byte 1

  wire::wr16_le(ev.data() + 2, seq);
  wire::wr32_le(ev.data() + 4, event);
  wire::wr32_le(ev.data() + 8, window);

  (void)ctx.transport().sendEvent32(window, ev.data());
}
  
  
bool XProtoTransport::sendError32(const uint8_t e[32]) {
  return sendAll(e, 32);
}

bool XProtoTransport::sendErrorCore(uint8_t errorCode, uint16_t seq,
                                    uint32_t resourceId, uint8_t majorCode)
{
#ifndef NDEBUG
  fprintf(stderr, "[X11_ERROR] code=%u seq=%u rid=0x%X major=%u\n",
          (unsigned)errorCode, (unsigned)seq, resourceId, (unsigned)majorCode);
#endif
  const auto e = x11::wireerr::buildCoreError32(errorCode, seq, resourceId, majorCode);
  return sendAll(e.data(), e.size());
}
  
  
} // namespace x11
