//
//  XProtoServer.cpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "XProtoServer.hpp"

#include <unordered_map>

namespace x11 {

struct XProtoServer::Impl {
  std::unordered_map<uint32_t, WindowView> testWindows;
};

XProtoServer::XProtoServer()
: ctx_()
, eventOps_(ctx_)
, transport_(ctx_, eventOps_)
, impl_(new Impl())
{
  // Wire transport into context so EventOps can reach it if desired.
  ctx_.setTransport(&transport_);

  // Default: context window lookup calls back into this instance.
  ctx_.setWindowLookup(&XProtoServer::lookupWindowTrampoline, this);
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
