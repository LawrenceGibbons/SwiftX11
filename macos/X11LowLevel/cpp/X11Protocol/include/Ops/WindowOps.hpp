//
//  WindowOps.hpp
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

// WindowOps: Create/Destroy/Map/Unmap/Configure/QueryTree/GetGeometry/GetWindowAttributes
class WindowOps {
public:
  WindowOps(XProtoServerState& state, XProtoRequestContext& ctx)
  : state_(state), ctx_(ctx) {}

  // CreateWindow (major = 1) — no reply
  void handleCreateWindow(uint8_t depth,
                          const uint8_t* payload, std::size_t len);

  // ChangeWindowAttributes (major = 2) — no reply
  void handleChangeWindowAttributes(const uint8_t* payload, std::size_t len);

  // GetWindowAttributes (major = 3) — reply
  void handleGetWindowAttributes(int clientFd, uint16_t seq,
                                 const uint8_t* payload, std::size_t len);

  // DestroyWindow (major = 4) — no reply
  void handleDestroyWindow(const uint8_t* payload, std::size_t len);

  // MapWindow (major = 8) — no reply (but may generate Expose)
  void handleMapWindow(int clientFd, uint16_t seq,
                       const uint8_t* payload, std::size_t len);

  // MapSubwindows (major = 9) — no reply (may generate Expose)
  void handleMapSubwindows(int clientFd, uint16_t seq,
                           const uint8_t* payload, std::size_t len);

  // UnmapWindow (major = 10) — no reply
  void handleUnmapWindow(const uint8_t* payload, std::size_t len);

  // ConfigureWindow (major = 12) — no reply (may generate ConfigureNotify/Expose)
  void handleConfigureWindow(int clientFd, uint16_t seq,
                             const uint8_t* payload, std::size_t len);

  // GetGeometry (major = 14) — reply
  void handleGetGeometry(int clientFd, uint16_t seq,
                         const uint8_t* payload, std::size_t len);

  // QueryTree (major = 15) — reply
  void handleQueryTree(int clientFd, uint16_t seq,
                       const uint8_t* payload, std::size_t len);

private:
  XProtoServerState& state_;
  XProtoRequestContext& ctx_;
};
