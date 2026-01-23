//
//  ShapeOps.hpp
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

// ShapeOps: PolyFillRectangle / PolyFillArc
class ShapeOps {
public:
  ShapeOps(XProtoServerState& state, XProtoRequestContext& ctx)
  : state_(state), ctx_(ctx) {}

  // PolyFillRectangle (major = 70) — no reply
  void handlePolyFillRectangle(int clientFd, uint16_t seq,
                               const uint8_t* payload, std::size_t len);

  // PolyFillArc (major = 71) — no reply
  void handlePolyFillArc(int clientFd, uint16_t seq,
                         const uint8_t* payload, std::size_t len);

private:
  XProtoServerState& state_;
  XProtoRequestContext& ctx_;
};
