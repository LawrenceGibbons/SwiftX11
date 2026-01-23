//
//  QueryOps.hpp
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

// QueryOps: “query/reply-ish” core protocol requests + a few no-reply queries
// Keep this focused on things that primarily build replies.
class QueryOps {
public:
  QueryOps(XProtoServerState& state, XProtoRequestContext& ctx)
  : state_(state), ctx_(ctx) {}

  // QueryExtension (major = 98) — reply
  void handleQueryExtension(int clientFd, uint16_t seq);

  // ListExtensions (major = 99) — reply
  void handleListExtensions(int clientFd, uint16_t seq);

  // QueryColors (major = 91) — reply
  void handleQueryColors(int clientFd, uint16_t seq,
                         const uint8_t* payload, std::size_t len);

  // QueryPointer (major = 38) — reply
  void handleQueryPointer(int clientFd, uint16_t seq,
                          const uint8_t* payload, std::size_t len);

  // GetInputFocus (major = 43) — reply
  void handleGetInputFocus(int clientFd, uint16_t seq);

private:
  XProtoServerState& state_;
  XProtoRequestContext& ctx_;
};
