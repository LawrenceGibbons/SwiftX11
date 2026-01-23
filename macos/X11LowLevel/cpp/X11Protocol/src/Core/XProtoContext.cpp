//
//  XProtoContext.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "XProtoContext.hpp"
#include "XProtoTransport.hpp"

#include <cstdio>
#include <cstdlib>

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
  if (!lookup_) return nullptr;
  scratch_ = WindowView{}; // reset
  if (!lookup_(xid, &scratch_, lookup_user_)) return nullptr;
  return &scratch_;
}

} // namespace x11
