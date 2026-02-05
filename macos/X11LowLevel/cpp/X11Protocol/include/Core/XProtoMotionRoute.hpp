//
//  XProtoMotionRoute.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/4/26.
//

#pragma once
#include <cstdint>

namespace x11 { 
  
  class XProtoContext;
  
  uint32_t pick_motion_target(x11::XProtoContext& ctx, uint32_t host_xid, int32_t x_px, int32_t y_px);
}
