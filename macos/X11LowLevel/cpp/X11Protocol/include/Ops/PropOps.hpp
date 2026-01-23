//
//  PropOps.hpp
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

// PropOps: ChangeProperty / GetProperty
class PropOps {
public:
  PropOps(XProtoServerState& state, XProtoRequestContext& ctx)
  : state_(state), ctx_(ctx) {}

  // ChangeProperty (major = 18) — no reply
  void handleChangeProperty(uint8_t mode, const uint8_t* payload, std::size_t len);

  // GetProperty (major = 20) — reply
  void handleGetProperty(int clientFd, uint16_t seq, uint8_t deleteFlag,
                         const uint8_t* payload, std::size_t len);

private:
  XProtoServerState& state_;
  XProtoRequestContext& ctx_;
};
