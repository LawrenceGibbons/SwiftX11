//
//  XProtoContext.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstdio>
#include <cstdlib>
#include <cassert>

#include "Core/XProtoContext.hpp"
#include "Core/XClient.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Core/WindowTable.hpp"

namespace x11 {

void XProtoContext::setClient(XClient* c) {
  client_ = c;
  if (c) {
    transport_ = &c->transport();
    reply_ = &c->reply();
  }
}

void XProtoContext::clearClient() {
  client_ = nullptr;
  transport_ = nullptr;
  reply_ = nullptr;
}

void XProtoContext::tracef(const char* fmt, ...) {
#ifdef X11_TRACE_VERBOSE
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
#else
  (void)fmt;
#endif
}

XProtoTransport& XProtoContext::transport() {
  if (!transport_) {
#ifndef NDEBUG
    std::fprintf(stderr, "[XProtoContext] FATAL: transport() called but transport_ is null\n");
#endif
    std::abort();
  }
  return *transport_;
}

const WindowView* XProtoContext::window(uint32_t xid) {
  // 1) Prefer WindowTable if installed
  if (window_table_) {
    if (window_table_->snapshot(xid, scratch_)) return &scratch_;
  }

#ifndef NDEBUG
  tracef("[XProtoContext] window(0x%08X) fell back to lookup_ (WindowTable miss)\n", xid);
#endif

  // 2) Fallback to callback snapshot (old path)
  if (!lookup_) return nullptr;
  if (!lookup_(xid, &scratch_, lookup_user_)) return nullptr;
  return &scratch_;
}

const WindowTable& XProtoContext::windows() const {
  assert(window_table_ && "XProtoContext::windows(): window_table_ not set");
  return *window_table_;
}
  
WindowTable& XProtoContext::windows() {
  assert(window_table_ && "XProtoContext::windows(): window_table_ not set");
  return *window_table_;
}
    
} // namespace x11
