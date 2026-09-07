//
//  XKBOps.hpp
//  X11LowLevel
//
//  XKEYBOARD extension request handlers (major opcode ext::kXKB).
//  Phase F / M23 of docs/XI2_XORG_COMPARISON.md, v1.20.0.20.
//
//  The keymap itself lives in Core/XkbKeymap.hpp; this file is the protocol
//  front end: request parsing, per-client state (XClient::xkb()), device
//  spec resolution, error replies, and the reply-bearing / void split that
//  keeps XCB's sequence accounting intact.
//

#pragma once
#include <cstdint>
#include <Core/XProtoRegistrar.hpp>

namespace x11 {

class XProtoContext;

class XKBOps {
public:
  // Entry point from ExtensionOps::handle for major opcode ext::kXKB.
  static void dispatch(XProtoContext& ctx, DispatchContext& dc);
};

} // namespace x11
