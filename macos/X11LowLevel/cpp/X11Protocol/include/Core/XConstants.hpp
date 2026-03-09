//
//  XConstants.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/26/26.
//

#pragma once

namespace x11 {
  
  static constexpr uint32_t kRootXid = 0x00000001u;
  static constexpr uint16_t kDepth   = 24;
  // Note: kRootW/kRootH removed — use x11::getScreenLayout().virtual_w/h
  // for actual root window dimensions (dynamic, multi-monitor aware).
  
}
