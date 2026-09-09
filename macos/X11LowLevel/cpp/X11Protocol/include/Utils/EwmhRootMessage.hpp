//
//  EwmhRootMessage.hpp
//  X11LowLevel
//
//  R1 Phase 4 (Cluster A, A1 + EWMH ClientMessage interpretation).
//
//  A client asks the window manager to change a window's state by sending a
//  ClientMessage to the ROOT window with SubstructureRedirect|SubstructureNotify
//  in the event mask (EWMH "Root Window Messages", ICCCM WM_CHANGE_STATE).  On a
//  normal X server the WM is the one client that selected SubstructureRedirect
//  on the root and receives it; SwiftX11 *is* the (rootless) window manager, so
//  it interprets the message here and drives the backing NSWindow through the
//  existing UI-command bridge, instead of forwarding it to a client.
//
//  Header-only so it needs no new translation unit (no .xcodeproj change).
//
#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include "Core/PropertyTable.hpp"
#include "Core/ClipboardAtoms.hpp"
#include "Core/XConstants.hpp"
#include "Utils/WireLE.hpp"
#include "SwiftX11Bridge.h"

namespace x11 {
namespace ewmh {

// Synthetic "window type" codes carried over the existing x11_ui_push_window_type
// bridge (Swift's applyWindowType switches on them).  The 0x8000000x range is
// already used for MODAL/FULLSCREEN/undecorated (v1.12.0); Phase 4 adds the
// add/remove twins and the maximize / iconify / activate actions.  Kept here so
// the C++ side has one named list.
namespace uiaction {
  static constexpr uint32_t kModal            = 0x80000001u; // (existing)
  static constexpr uint32_t kFullscreenAdd    = 0x80000002u; // (existing)
  static constexpr uint32_t kUndecorated      = 0x80000003u; // (existing)
  static constexpr uint32_t kMaximizeAdd      = 0x80000004u;
  static constexpr uint32_t kMaximizeRemove   = 0x80000014u;
  static constexpr uint32_t kFullscreenRemove = 0x80000012u;
  static constexpr uint32_t kIconify          = 0x80000007u;
  static constexpr uint32_t kActivate         = 0x80000008u;
}

// True for the ClientMessage types SwiftX11 interprets on the root.
inline bool isKnownRootMessage(uint32_t msgType) {
  return msgType == atom::k_NET_ACTIVE_WINDOW ||
         msgType == atom::k_NET_WM_STATE ||
         msgType == atom::kWM_CHANGE_STATE;
}

// Resolve a ClientMessage's target window (its `window` field) to a live
// top-level; 0 when it is missing/None/root/unknown.
inline uint32_t resolveTarget(XProtoContext& ctx, uint32_t window) {
  if (window == 0 || window == x11::kRootWindowXid) return 0;
  if (!ctx.windows().exists(window)) return 0;
  uint32_t host = ctx.windows().topLevelAncestorOf(window);
  return host ? host : window;
}

// Interpret one ClientMessage sent to the root.  `ev` is the 32-byte wire event
// (already SendEvent-flagged); format is ev[1] (EWMH uses 32 → five CARD32s at
// ev+12).  Increment 1 wires _NET_ACTIVE_WINDOW; _NET_WM_STATE and
// WM_CHANGE_STATE land in increment 2 (recognised here, acted on there).
inline void handleRootClientMessage(XProtoContext& ctx, const uint8_t ev[32]) {
  const uint32_t window  = wire::rd32_le(ev + 4);
  const uint32_t msgType = wire::rd32_le(ev + 8);

  if (msgType == atom::k_NET_ACTIVE_WINDOW) {
    const uint32_t host = resolveTarget(ctx, window);
    if (!host) return;
    // Activate: raise, make key, and deminiaturize if minimised.  raiseWindow
    // already orders-front + makeKey for a mapped window; the deminiaturize is
    // handled on the Swift side (the activate path un-minimises first).
    x11_ui_push_window_type(host, uiaction::kActivate);
#ifndef NDEBUG
    { char b[96]; snprintf(b, sizeof b, "[EWMH] _NET_ACTIVE_WINDOW win=0x%X host=0x%X\n",
                           (unsigned)window, (unsigned)host); x11_ui_push_log(2, b); }
#endif
    return;
  }

  // _NET_WM_STATE and WM_CHANGE_STATE: recognised now, interpreted in
  // increment 2 (maximize / fullscreen / iconify).
#ifndef NDEBUG
  { char b[96]; snprintf(b, sizeof b, "[EWMH] root ClientMessage type=%u win=0x%X (unhandled yet)\n",
                         (unsigned)msgType, (unsigned)window); x11_ui_push_log(2, b); }
#endif
}

// Publish the EWMH support advertisement on the root window so clients (wmctrl,
// GTK, Java's XToolkit) recognise a compliant WM and send the messages above.
// _NET_SUPPORTING_WM_CHECK points root at itself (root carries _NET_WM_NAME),
// which satisfies the two-hop compliance check without a dedicated child window.
inline void initRootProperties() {
  auto put32 = [](uint32_t wid, uint32_t atom, uint32_t type,
                  const std::vector<uint32_t>& vals) {
    std::vector<uint8_t> bytes(vals.size() * 4);
    for (size_t i = 0; i < vals.size(); i++) wire::wr32_le(bytes.data() + i * 4, vals[i]);
    PropertyTable::instance().setReplace(x11::kRootWindowXid, atom, type, 32,
                                         bytes.data(), bytes.size());
  };

  put32(x11::kRootWindowXid, atom::k_NET_SUPPORTED, atom::kATOM, {
    atom::k_NET_SUPPORTED,
    atom::k_NET_SUPPORTING_WM_CHECK,
    atom::k_NET_ACTIVE_WINDOW,
    atom::k_NET_WM_NAME,
    atom::k_NET_WM_WINDOW_TYPE,
    atom::k_NET_WM_STATE,
    atom::k_NET_WM_STATE_MODAL,
    atom::k_NET_WM_STATE_FULLSCREEN,
    atom::k_NET_WM_STATE_MAXIMIZED_VERT,
    atom::k_NET_WM_STATE_MAXIMIZED_HORZ,
    atom::k_NET_WM_STATE_HIDDEN,
    atom::k_NET_FRAME_EXTENTS,
  });

  // _NET_SUPPORTING_WM_CHECK: root → root (the check window is itself).
  put32(x11::kRootWindowXid, atom::k_NET_SUPPORTING_WM_CHECK, /*WINDOW*/33,
        { x11::kRootWindowXid });

  // _NET_WM_NAME on the check window (root): the WM's name, UTF8_STRING.
  static const char kName[] = "SwiftX11";
  PropertyTable::instance().setReplace(x11::kRootWindowXid, atom::k_NET_WM_NAME,
                                       atom::kUTF8_STRING, 8,
                                       reinterpret_cast<const uint8_t*>(kName),
                                       sizeof(kName) - 1);
}

} // namespace ewmh
} // namespace x11
