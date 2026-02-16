//
//  InputRouting.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/15/26.
//

// Core/InputRouting.hpp
#pragma once
#include <cstdint>

namespace x11 {
  
  class XProtoContext;
  
  uint32_t pickDeepestMappedWindowAtHostPoint(XProtoContext& ctx, uint32_t host_xid, int32_t host_x, int32_t host_y);
  
}

