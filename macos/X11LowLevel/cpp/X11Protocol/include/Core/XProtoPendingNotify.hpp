//
//  XProtoPendingNotify.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/21/26.
//

#pragma once
#include <cstdint>
#include <type_traits>

namespace x11 {

struct PendingNotify {
  uint32_t wid = 0;
  uint8_t  want_configure = 0;
  uint8_t  want_expose = 0;
  uint8_t  _pad[2] = {0, 0};
  
  // new:
  uint8_t  expose_has_rect = 0;   // 0 => full window
  uint8_t  _pad_rect[1] = {0}; // explicit pad to keep layout stable
  uint16_t expose_x = 0;
  uint16_t expose_y = 0;
  uint16_t expose_w = 0;
  uint16_t expose_h = 0;
  uint16_t expose_count = 0;  
};

  static_assert(std::is_trivially_copyable_v<PendingNotify>, "PendingNotify must be trivially copyable");
  static_assert(std::is_standard_layout_v<PendingNotify>, "PendingNotify should be standard-layout");

  // New expected size/alignment for the rect-carrying version.
  static_assert(sizeof(PendingNotify) == 20, "Unexpected PendingNotify size; check padding/alignment");
  static_assert(alignof(PendingNotify) == alignof(uint32_t), "Unexpected alignment for PendingNotify");
} // namespace x11
