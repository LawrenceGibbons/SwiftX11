//
//  XProtoServer.cpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstdio>
#include <unordered_map>
#include <exception>

#include "Ops/ReplyWriter.hpp"
#include "Core/XProtoServer.hpp"
#include "Core/SurfaceDesc.hpp"
#include "Utils/ByteReader.hpp"
#include "Ops/EventOps.hpp"
#include "UI/UICommandQueue.hpp"
#include "x11_server_internal.h"
#include "x11_backend_fb.h"



static void fb_event_observer(uint32_t xid, uint16_t w, uint16_t h,
                              const char* op, const char* why,
                              const char* file, int line, void* user)
{
  auto* wt = static_cast<x11::WindowTable*>(user);
  if (wt) wt->noteFbResizeDbg(xid, why, file, line);
  // optional: also print op/w/h here if you want
}


static inline uint16_t rd16_le(const uint8_t* p) {
  return (uint16_t)p[0] | (uint16_t(p[1]) << 8);
}
static inline uint16_t rd16_be(const uint8_t* p) {
  return (uint16_t)p[1] | (uint16_t(p[0]) << 8);
}
static inline uint32_t rd32_le(const uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
}
static inline uint32_t rd32_be(const uint8_t* p) {
  return (uint32_t)p[3] | (uint32_t(p[2])<<8) | (uint32_t(p[1])<<16) | (uint32_t(p[0])<<24);
}

extern "C" void x11_cpp_set_window_table(const x11::WindowTable* wt);

namespace x11 {

static x11::UICommandQueue g_ui; // (temporary global for 2A)
  
struct XProtoServer::Impl {
  std::unordered_map<uint32_t, WindowView> testWindows;
};

XProtoServer::XProtoServer()
: ctx_()
, eventOps_(std::make_unique<EventOps>(ctx_))
, transport_(ctx_, *eventOps_)
, reply_(transport_)
, impl_(std::make_unique<Impl>())
{
  table_.fill(Entry{nullptr, nullptr});

  // Wire transport and Replywrite into context so EventOps can reach it if desired.
  ctx_.setTransport(&transport_);
  ctx_.setReplyWriter(&reply_);
  ctx_.setWindowTable(&windows_);
  ctx_.setPixmapTable(&pixmapTable_);
  ctx_.setUI(&g_ui);
  ctx_.setFontTable(&fonts_);
  ctx_.setCursorTable(&cursors_);
  ctx_.setSurfaceRegistry(&surfaces_);
  
  // Default: context window lookup calls back into this instance.
  ctx_.setWindowLookup(&XProtoServer::lookupWindowTrampoline, this);
  x11_backend_fb_set_event_observer(&fb_event_observer, &windows_);

  
  // load fonts
  std::string err;
  if (!fonts_.loadBuiltins(&err)) {
    ctx_.tracef("[FontTable] loadBuiltins failed: %s\n", err.c_str());
  }
  
  x11_cpp_set_window_table(&ctx_.windows());
}

x11::XProtoServer::~XProtoServer() = default;
  

// ----------------------------------------------
int XProtoServer::dispatch(uint8_t major, uint8_t minor, uint16_t seq,
                           const uint8_t* payload, std::size_t remain)
{
  ByteReader br(payload, remain);
  
#ifndef NDEBUG
  // Put this right after you decode major/minor/seq:
  if (major == 70 || // PolyFillRectangle
      major == 62 || // CopyArea
      major == 63 || // CopyPlane
      major == 72 || // PutImage
      major == 74 || // PolyText8
      major == 75 || // PolyText16
      major == 76 || // ImageText8
      major == 77 || // ImageText16
      major == 65 || // PolyLine (underline cursor sometimes)
      major == 66)   // PolySegment
  {
    fprintf(stderr, "[WATCH] major=%u minor=%u seq=%u remain=%zu\n",
            (unsigned)major, (unsigned)minor, (unsigned)seq, br.remaining());
  }
#endif
  
#ifndef NDEBUG
  fprintf(stderr, "[DISPATCH] major=%u minor=%u seq=%u remain=%zu\n",
          (unsigned)major, (unsigned)minor, (unsigned)seq, remain);
#endif
  if ( major == 91 ) {
#ifndef NDEBUG
    {
      const uint8_t* p = br.ptr();
      const size_t n = br.remaining();
      if (n >= 12) {
        uint32_t cmap_le = rd32_le(p+0), cmap_be = rd32_be(p+0);
        uint16_t n_le    = rd16_le(p+4), n_be    = rd16_be(p+4);
        uint32_t pix_le  = rd32_le(p+8), pix_be  = rd32_be(p+8);
        
        fprintf(stderr,
                "[QC SANITY] seq=%u raw=%02X%02X%02X%02X %02X%02X %02X%02X %02X%02X%02X%02X "
                "cmap(le=%08X be=%08X) n(le=%u be=%u) pix(le=%08X be=%08X)\n",
                (unsigned)seq,
                p[0],p[1],p[2],p[3], p[4],p[5], p[6],p[7], p[8],p[9],p[10],p[11],
                (unsigned)cmap_le, (unsigned)cmap_be,
                (unsigned)n_le, (unsigned)n_be,
                (unsigned)pix_le, (unsigned)pix_be
                );
      }
    }
    {
      const uint8_t* p = br.ptr();
      if (br.remaining() >= 6) {
        // Copy reader so we don’t advance the real one
        ByteReader tmp = br;
        tmp.skip(4);
        uint16_t v = tmp.readU16();
        fprintf(stderr, "[QC READU16] bytes=%02X%02X -> readU16()=%u\n",
                p[4], p[5], (unsigned)v);
      }
    }
#endif
  }
  
  // If we don't have a handler registered for this major opcode, log a message
  const Entry& e = table_[major];
  transport_.last_request_major_ = major;
  transport_.last_request_minor_ = minor;
  transport_.last_request_seq_   = seq;
  if (!e.fn) {
#ifndef NDEBUG
    static uint64_t last_warn_ns = 0;
    uint64_t now = x11_now_ns();
    if (now - last_warn_ns > 500000000ULL) { // 0.5s throttle
      last_warn_ns = now;
      fprintf(stderr,
              "[DISPATCH] UNHANDLED major=%u minor=%u seq=%u (no C++ handler)\n",
              (unsigned)major,
              (unsigned)minor,
              (unsigned)seq);
    }
#endif
    return 0;
  }

  DispatchContext dc{
    .major = major,
    .minor = minor,      // really “data/detail” for core
    .seq   = seq,
    .br    = br
  };

  try {
    e.fn(e.user, ctx_, dc);

    // If handler didn't consume everything, swallow remainder to keep stream aligned.
    // (Some handlers deliberately ignore padding; this keeps us safe.)
    if (dc.br.remaining() > 0) {
      dc.br.skip(dc.br.remaining());
    }

    return 1;
  } catch (const std::exception& ex) {
#ifndef NDEBUG
    ctx_.tracef("[XProtoServer] dispatch EXCEPTION major=%u minor=%u seq=%u remain=%zu: %s\n",
                (unsigned)major, (unsigned)minor, (unsigned)seq, remain, ex.what());
#endif
    // Important: still “handled” because we must not run the C handler too.
    // We already advanced br inside the handler or threw; safest is to treat as handled.
    return 1;
  } catch (...) {
#ifndef NDEBUG
    ctx_.tracef("[XProtoServer] dispatch UNKNOWN EXCEPTION major=%u minor=%u seq=%u remain=%zu\n",
                (unsigned)major, (unsigned)minor, (unsigned)seq, remain);
#endif
    return 1;
  }
}
  

void XProtoServer::registerMajor(uint8_t major, HandlerFn fn, void* user) {
#ifndef NDEBUG
  if (table_[major].fn) {
    fprintf(stderr,
            "[REG] WARNING: major=%u already registered (old user=%p) -> overwriting with user=%p\n",
            (unsigned)major, table_[major].user, user);
  }
#endif
  table_[major].fn = fn;
  table_[major].user = user;
}
  
  
void XProtoServer::setWindowLookup(WindowLookupFn fn, void* user) {
  injected_lookup_ = fn;
  injected_lookup_user_ = user;

  // Keep ctx_'s callback pointing at this server; we route to injected fn inside lookupWindow().
  ctx_.setWindowLookup(&XProtoServer::lookupWindowTrampoline, this);
}

void XProtoServer::attachClientFd(int fd) {
  transport_.attachClientFd(fd);
}

void XProtoServer::setXprotoThreadSelf() {
  transport_.setXprotoThreadSelf();
}

void XProtoServer::noteLastSeq(uint16_t seq) {
  transport_.noteLastSeq(seq);
}

void XProtoServer::queueNotify(uint32_t wid, bool wantConfigure, bool wantExpose) {
  transport_.queueNotify(wid, wantConfigure, wantExpose);
}

void XProtoServer::flushNotifyQueue() {
  transport_.flushNotifyQueue();
}

void XProtoServer::setTestWindow(const WindowView& w) {
  if (!impl_) return;
  impl_->testWindows[w.xid] = w;
}

void XProtoServer::clearTestWindows() {
  if (!impl_) return;
  impl_->testWindows.clear();
}

bool XProtoServer::lookupWindowTrampoline(uint32_t xid, WindowView* out, void* user) {
  if (!user || !out) return false;
  return static_cast<XProtoServer*>(user)->lookupWindow(xid, out);
}

bool XProtoServer::lookupWindow(uint32_t xid, WindowView* out) {
  if (!out) return false;

  // 1) Prefer injected production lookup.
  if (injected_lookup_) {
    return injected_lookup_(xid, out, injected_lookup_user_);
  }

  // 2) Fall back to test map.
  if (!impl_) return false;
  auto it = impl_->testWindows.find(xid);
  if (it == impl_->testWindows.end()) return false;
  *out = it->second;
  return true;
}
  
void XProtoServer::updateSurface(uint32_t xid, const SurfaceDesc& s) {
  if (xid == 0) return;

  // Key choice for first cut: registry is keyed by HOST (top-level window).
  const uint32_t host = ctx_.windows().topLevelAncestorOf(xid); // you already have this method
  const uint32_t key  = host ? host : xid;

  ctx_.surfaces().set(key, s);
  ctx_.windows().setPresentable(key, true);

  // If anything was waiting to present, release it now.
  if (ctx_.windows().consumeDirtyIfReady(key)) {
    ctx_.ui().push(x11::UICommand{ x11::UICommand::Type::Damage, key });
  }
}

void XProtoServer::clearSurface(uint32_t xid) {
  if (xid == 0) return;
  const uint32_t host = ctx_.windows().topLevelAncestorOf(xid);
  const uint32_t key  = host ? host : xid;

  ctx_.surfaces().clear(key);
  ctx_.windows().setPresentable(key, false);
}


  
} // namespace x11

