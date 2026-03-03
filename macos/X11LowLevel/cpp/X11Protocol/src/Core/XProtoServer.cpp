//
//  XProtoServer.cpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstdio>
#include <unordered_map>
#include <exception>

#include "Core/XProtoServer.hpp"
#include "Core/XClient.hpp"
#include "Core/SurfaceDesc.hpp"
#include "Utils/ByteReader.hpp"
#include "Ops/EventOps.hpp"
#include "Core/timestamp.hpp"

extern "C" {
#include "SwiftX11Bridge.h"
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

namespace x11 {

struct XProtoServer::Impl {
  std::unordered_map<uint32_t, WindowView> testWindows;
};

XProtoServer::XProtoServer()
  : ctx_()
  , eventOps_(std::make_unique<EventOps>(ctx_))
  , impl_(std::make_unique<Impl>())
{
  table_.fill(Entry{nullptr, nullptr});

  // Wire server-wide state into context.
  ctx_.setWindowTable(&windows_);
  ctx_.setPixmapTable(&pixmapTable_);
  ctx_.setUI(&ui_);
  ctx_.setFontTable(&fonts_);
  ctx_.setCursorTable(&cursors_);
  ctx_.setSurfaceRegistry(&surfaces_);
  ctx_.setGrabTable(&grabs_);
  ctx_.setInputState(&input_);

  // Default: context window lookup calls back into this instance.
  ctx_.setWindowLookup(&XProtoServer::lookupWindowTrampoline, this);

  // Load built-in fonts.
  std::string err;
  if (!fonts_.loadBuiltins(&err)) {
    ctx_.tracef("[FontTable] loadBuiltins failed: %s\n", err.c_str());
  }

}

x11::XProtoServer::~XProtoServer() = default;


// ----------------------------------------------
int XProtoServer::dispatch(uint8_t major, uint8_t minor, uint16_t seq,
                           const uint8_t* payload, std::size_t remain)
{
  ByteReader br(payload, remain);

#ifdef X11_TRACE_VERBOSE
  if (major == 70 || major == 68 || major == 71 || major == 62 ||
      major == 63 || major == 72 || major == 74 || major == 75 ||
      major == 76 || major == 77 || major == 65 || major == 66)
  {
    fprintf(stderr, "[WATCH] major=%u minor=%u seq=%u remain=%zu\n",
            (unsigned)major, (unsigned)minor, (unsigned)seq, br.remaining());
  }
  fprintf(stderr, "[DISPATCH] major=%u minor=%u seq=%u remain=%zu\n",
          (unsigned)major, (unsigned)minor, (unsigned)seq, remain);
#endif
  if ( major == 91 ) {
#ifdef X11_TRACE_VERBOSE
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
        ByteReader tmp = br;
        tmp.skip(4);
        uint16_t v = tmp.readU16();
        fprintf(stderr, "[QC READU16] bytes=%02X%02X -> readU16()=%u\n",
                p[4], p[5], (unsigned)v);
      }
    }
#endif
  }


  const Entry& e = table_[major];
  // Track last request in the transport (if a client is connected).
  if (ctx_.hasClient()) {
    ctx_.transport().last_request_major_ = major;
    ctx_.transport().last_request_minor_ = minor;
    ctx_.transport().last_request_seq_   = seq;
  }
  if (!e.fn) {
#ifndef NDEBUG
    static uint64_t last_warn_ns = 0;
    uint64_t now = x11_now_ns_monotonic();
    if (now - last_warn_ns > 500000000ULL) {
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
    .minor = minor,
    .seq   = seq,
    .br    = br
  };

  try {
    e.fn(e.user, ctx_, dc);

    if (dc.br.remaining() > 0) {
      dc.br.skip(dc.br.remaining());
    }

    return 1;
  } catch (const std::exception& ex) {
#ifndef NDEBUG
    ctx_.tracef("[XProtoServer] dispatch EXCEPTION major=%u minor=%u seq=%u remain=%zu: %s\n",
                (unsigned)major, (unsigned)minor, (unsigned)seq, remain, ex.what());
#endif
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
  ctx_.setWindowLookup(&XProtoServer::lookupWindowTrampoline, this);
}

void XProtoServer::queueNotify(uint32_t wid, bool wantConfigure, bool wantExpose) {
  if (!ctx_.hasClient()) return;
  ctx_.transport().queueNotify(wid, wantConfigure, wantExpose);
}

void XProtoServer::flushNotifyQueue() {
  if (!ctx_.hasClient()) return;
  ctx_.transport().flushNotifyQueue();
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

  if (injected_lookup_) {
    return injected_lookup_(xid, out, injected_lookup_user_);
  }

  if (!impl_) return false;
  auto it = impl_->testWindows.find(xid);
  if (it == impl_->testWindows.end()) return false;
  *out = it->second;
  return true;
}

void XProtoServer::updateSurface(uint32_t xid, const SurfaceDesc& s) {
  if (xid == 0) return;

  const uint32_t host = ctx_.windows().topLevelAncestorOf(xid);
  const uint32_t key  = host ? host : xid;

#ifdef X11_TRACE_VERBOSE
  fprintf(stderr, "[UPDATE_SURFACE] xid=0x%08X -> host/key=0x%08X wh=%ux%u bpr=%u ptr=%p\n",
          (unsigned)xid, (unsigned)key,
          (unsigned)s.w, (unsigned)s.h, (unsigned)s.bytesPerRow, s.ptr);
#endif

  ctx_.surfaces().set(key, s);
  ctx_.windows().setPresentable(key, true);

  {
    x11::WindowView uv{};
    if (ctx_.windows().snapshot(key, uv)) {
      x11_shared_damage_union(key, 0, 0, (int32_t)uv.w, (int32_t)uv.h);
      x11_ui_push_damage(key, 0, 0, (int32_t)uv.w, (int32_t)uv.h);
    }
  }
}

void XProtoServer::clearSurface(uint32_t xid) {
  if (xid == 0) return;
  const uint32_t host = ctx_.windows().topLevelAncestorOf(xid);
  const uint32_t key  = host ? host : xid;

  ctx_.surfaces().clear(key);
  ctx_.windows().setPresentable(key, false);
  x11_shared_damage_clear(key);
}

} // namespace x11
