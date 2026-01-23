//
//  DrawOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once
#include <cstddef>
#include <cstdint>

// Forward declarations of core types the dispatcher will own.
// We'll define these later in a shared header (e.g., XProtoTypes.h).
struct XProtoServerState;
struct XProtoRequestContext;

class DrawOps {
public:
  DrawOps(XProtoServerState& state, XProtoRequestContext& ctx)
  : state_(state), ctx_(ctx) {}

  // Core opcode family: drawing-related requests
  void handlePutImage(uint8_t format, const uint8_t* payload, std::size_t len);
  void handleCopyArea(const uint8_t* payload, std::size_t len);
  void handleCopyPlane(const uint8_t* payload, std::size_t len);

private:
  XProtoServerState& state_;
  XProtoRequestContext& ctx_;
};

// Note: I included dummy empty definitions for XProtoServerState / XProtoRequestContext
// in the .cpp so the stub compiles even before you’ve created shared headers. Once you 
// add the real shared header, delete those dummy structs and #include the real one.
