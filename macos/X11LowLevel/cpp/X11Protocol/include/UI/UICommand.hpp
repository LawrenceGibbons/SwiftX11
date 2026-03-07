//
//  UICommand.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/22/26.
//

// UI/UICommand.hpp
#pragma once
#include <cstdint>

namespace x11 {

struct UICommand {
  enum class Type : uint8_t {
    Create, Destroy, Map, Unmap, Configure, SetTitle, Damage, Presentable, SetCursor,
    // later: DamageRects, etc.
  };

  Type type{};
  uint32_t xid = 0;
  uint32_t parent = 0;

  // geometry (position used for override-redirect windows)
  int32_t x_px = 0;
  int32_t y_px = 0;
  int32_t w_px = 0;
  int32_t h_px = 0;

  // flags (X11_UI_FLAG_* bits, used by Create)
  uint32_t flags = 0;

  // cursor
  uint32_t cursor_xid = 0;
  int32_t shape = 0;

  // title: pointer must remain valid until drained (or store fixed-size inline buffer)
  const char* title_utf8 = nullptr;
};

} // namespace x11
