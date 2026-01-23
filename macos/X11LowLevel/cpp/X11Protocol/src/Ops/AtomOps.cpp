//
//  AtomOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "AtomOps.hpp"

// Temporary stand-ins so this compiles before you introduce shared headers.
// Delete these once XProtoServerState / XProtoRequestContext exist for real.
struct XProtoServerState {};
struct XProtoRequestContext {};

void AtomOps::handleInternAtom(int /*clientFd*/, uint16_t /*seq*/,
                               const uint8_t* /*payload*/, std::size_t /*len*/,
                               bool /*onlyIfExists*/) {
  // TODO:
  // Request body after 4-byte header:
  //   CARD16 name_len
  //   CARD16 pad
  //   name bytes padded to 4
  //
  // Behavior:
  // - If name already exists, return its atom id
  // - If onlyIfExists and missing, return 0 (None)
  // - Else allocate a new atom id (avoid 1..68 predefined collisions)
  //
  // Reply body (32 bytes total):
  // - atom id in bytes 8..11 (CARD32)
}

void AtomOps::handleGetAtomName(int /*clientFd*/, uint16_t /*seq*/,
                                const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO:
  // Request body:
  //   CARD32 atom
  //
  // Reply:
  // - name_len at bytes 8..9 (CARD16)
  // - extra length_words = padded_name_len / 4
  // - then name bytes + padding to 4
  //
  // Missing atom => empty string is acceptable for bring-up.
}
