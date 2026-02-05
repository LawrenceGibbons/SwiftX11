//
//  XProtoNotifyBridge.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/4/26.
//

#pragma once
#include <cstdint>

namespace x11::notify {

// Called on xproto thread (or anywhere, if internally queued).
void postMotion(uint32_t host_xid,
                int32_t win_x, int32_t win_y,
                int32_t root_x, int32_t root_y,
                uint8_t deliver,
                uint32_t buttons, uint32_t mods);

void postButtonLegacy(uint32_t xid, int is_press, int32_t x_px, int32_t y_px, uint32_t buttons, uint32_t mods);

} // namespace x11::notify
