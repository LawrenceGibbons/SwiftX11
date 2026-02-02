//
//  HostResize.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/2/26.
//

#pragma once
#include <cstdint>

namespace x11 {
class XProtoContext;

// Called on the server/protocol thread when the host resized a rootless window surface.
void applyRootlessResize(XProtoContext& ctx, uint32_t wid, int32_t w_px, int32_t h_px);

} // namespace x11
