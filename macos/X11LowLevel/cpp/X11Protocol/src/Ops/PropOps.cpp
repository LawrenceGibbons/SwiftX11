//
//  PropOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "PropOps.hpp"

// Temporary stand-ins so this compiles before you introduce shared headers.
// Delete these once XProtoServerState / XProtoRequestContext exist for real.
struct XProtoServerState {};
struct XProtoRequestContext {};

void PropOps::handleChangeProperty(uint8_t /*mode*/, const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO: ChangeProperty (major=18)
  // - parse window, property atom, type atom, format(8/16/32), nUnits
  // - apply mode: Replace / Prepend / Append
  // - store in server-side property table keyed by (window, atom)
  // - optional: best-effort title extraction for WM_NAME / _NET_WM_NAME
}

void PropOps::handleGetProperty(int /*clientFd*/, uint16_t /*seq*/, uint8_t /*deleteFlag*/,
                                const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO: GetProperty (major=20)
  // - parse window, property atom, requested type, longOffset/longLength
  // - if missing: reply format=0 type=None length=0
  // - else: slice bytes, pad to 4, set bytesAfter + nItems correctly
  // - if deleteFlag and returning entire property at offset 0: delete it
  // NOTE: actual socket write must happen on the xproto thread (same as your current model).
}
