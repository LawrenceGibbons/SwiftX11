//
//  XProtoContext.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstdio>
#include <cstdlib>
#include <cassert>

#include "XProtoContext.hpp"
#include "ReplyWriter.hpp"
#include "XProtoTransport.hpp"
#include "WindowTable.hpp"

namespace x11 {

void XProtoContext::tracef(const char* fmt, ...) {
#ifndef NDEBUG
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
