//
//  QueryOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "QueryOps.hpp"

// Temporary stand-ins so this compiles before you introduce shared headers.
// Delete these once XProtoServerState / XProtoRequestContext exist for real.
struct XProtoServerState {};
struct XProtoRequestContext {};

void QueryOps::handleQueryExtension(int /*clientFd*/, uint16_t /*seq*/) {
  // TODO:
  // - parse extension name from request payload (this signature may expand)
  // - reply present=0 for unsupported extensions (XInput will be missing initially)
  // - later: return present=1 + major_opcode/first_event/first_error for supported ones
}

void QueryOps::handleListExtensions(int /*clientFd*/, uint16_t /*seq*/) {
  // TODO:
  // - reply with nExtensions + packed name list (length-prefixed strings)
  // - initially can return empty list to keep clients moving
}

void QueryOps::handleQueryColors(int /*clientFd*/, uint16_t /*seq*/,
                                 const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - request includes colormap + list of pixels
  // - reply includes list of xrgb entries (8 bytes each)
  // - for bring-up: treat pixel==0 as black, nonzero as white
  // - later: implement colormap/visual logic
}

void QueryOps::handleQueryPointer(int /*clientFd*/, uint16_t /*seq*/,
                                  const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // - reply includes root, child, rootX/rootY, winX/winY, key/button mask
  // - for bring-up: can use synthetic pointer state from server state
  // - later: hook to real host pointer events routed via backend
}

void QueryOps::handleGetInputFocus(int /*clientFd*/, uint16_t /*seq*/) {
  // TODO:
  // - reply with focus window XID + revert-to
  // - for bring-up: return root or last-focused
}
