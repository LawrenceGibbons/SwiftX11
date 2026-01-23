//
//  AtomOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once
#include <cstddef>
#include <cstdint>

// Forward declarations (real definitions will live in shared protocol headers later).
struct XProtoServerState;
struct XProtoRequestContext;

// AtomOps: core protocol atom name/interning.
// Keep “atom table” ownership in state_ (or a dedicated AtomTable later),
// but route the protocol request/response building here.
class AtomOps {
public:
  AtomOps(XProtoServerState& state, XProtoRequestContext& ctx)
  : state_(state), ctx_(ctx) {}

  // InternAtom (major = 16) — reply
  void handleInternAtom(int clientFd, uint16_t seq,
                        const uint8_t* payload, std::size_t len,
                        bool onlyIfExists);

  // GetAtomName (major = 17) — reply
  void handleGetAtomName(int clientFd, uint16_t seq,
                         const uint8_t* payload, std::size_t len);

private:
  XProtoServerState& state_;
  XProtoRequestContext& ctx_;
};
