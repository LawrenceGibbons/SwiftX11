//
//  XProtoServer.cpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstdio>
#include <unordered_map>
#include <exception>

#include "ReplyWriter.hpp"
#include "XProtoServer.hpp"
#include "ByteReader.hpp"
#include "EventOps.hpp"


namespace x11 {

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
  
  // Default: context window lookup calls back into this instance.
  ctx_.setWindowLookup(&XProtoServer::lookupWindowTrampoline, this);
}

x11::XProtoServer::~XProtoServer() = default;
  

int XProtoServer::dispatch(uint8_t major, uint8_t minor, uint16_t seq,
                           const uint8_t* payload, std::size_t remain)
{
  ByteReader br(payload, remain);

  // If we don't have a handler registered for this major opcode, caller should fall back to C.
  const Entry& e = table_[major];
  if (!e.fn) {
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
  
  
} // namespace x11

