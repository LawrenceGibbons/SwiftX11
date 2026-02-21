//
//  CursorRouting.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/20/26.
//

#pragma once
#include <cstdint>

#include "XProtoContext.hpp"
#include "Core/WindowTable.hpp"

namespace x11 {
  
  class XProtoContext;
  
  void maybeApplyCursor(XProtoContext& ctx, uint32_t host, uint32_t target);
  
  // Returns the effective cursor resource id for `start_xid`.
  // Semantics:
  //   - 0 => default/inherit
  //   - if window has cursor_xid != 0, use it
  //   - else inherit from parent chain until root
  //   - unknown xid/chain => 0
  static inline uint32_t resolveEffectiveCursorCid(XProtoContext& ctx, uint32_t start_xid)
  {
    if (start_xid == 0) return 0;
    return ctx.windows().cursor(start_xid); // WindowTable handles inheritance
  }

}
