//
//  XConstants.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/26/26.
//

#pragma once

#include <cstdint>   // uint32_t/uint16_t — the header must be self-sufficient (surfaced by R1 Phase 1c)

namespace x11 {

  // R1 (Cluster A) — the root window's identity and the PointerRoot/InputFocus
  // wire sentinels used to be one value (1), which misrouted SendEvent-to-root
  // and conflated "PointerRoot focus" with "focus is the root window."  They
  // are split apart here:
  //
  //   kRootWindowXid    — the real root window (a first-class WindowTable entry).
  //   kPointerRootFocus — the PointerRoot focus sentinel (wire value 1): focus
  //                       follows the pointer.  Distinct from the root window.
  //
  // Phase 1a/1b left BOTH at 0x1 so reclassifying every
  // former `kRootXid` site was a pure no-op; Phase 1c (now) flips kRootWindowXid to
  // 0x2 (advertised in the setup block, backed by a root WindowView) — that is
  // the only behavioural change.  0x2 is free: the server owns 0x1/0x20/0x21
  // (root/cmap/visual) and client XIDs come from higher rid ranges.
  static constexpr uint32_t kRootWindowXid    = 0x00000002u;  // Phase 1c: off the 0/1 wire sentinels
  static constexpr uint32_t kPointerRootFocus = 0x00000001u;  // PointerRoot (wire 1), stays 1

  static constexpr uint16_t kDepth   = 24;
  // Note: kRootW/kRootH removed — use x11::getScreenLayout().virtual_w/h
  // for actual root window dimensions (dynamic, multi-monitor aware).
  
}
