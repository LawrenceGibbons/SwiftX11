//
//  XI2EventMask.hpp
//  X11Protocol
//
//  XI2 (XInput2) event type constants and mask bits.
//

#pragma once

#include <cstdint>

namespace x11::xi2 {

// --- XI2 event types ---
constexpr uint16_t kKeyPress       = 2;
constexpr uint16_t kKeyRelease     = 3;
constexpr uint16_t kButtonPress    = 4;
constexpr uint16_t kButtonRelease  = 5;
constexpr uint16_t kMotion         = 6;
constexpr uint16_t kEnter          = 7;
constexpr uint16_t kLeave          = 8;
constexpr uint16_t kFocusIn        = 9;
constexpr uint16_t kFocusOut       = 10;

// --- XI2 event selection masks (bit = 1 << event_type) ---
constexpr uint32_t kKeyPressMask      = (1u << 2);
constexpr uint32_t kKeyReleaseMask    = (1u << 3);
constexpr uint32_t kButtonPressMask   = (1u << 4);
constexpr uint32_t kButtonReleaseMask = (1u << 5);
constexpr uint32_t kMotionMask        = (1u << 6);
constexpr uint32_t kEnterMask         = (1u << 7);
constexpr uint32_t kLeaveMask         = (1u << 8);
constexpr uint32_t kFocusInMask       = (1u << 9);
constexpr uint32_t kFocusOutMask      = (1u << 10);

// --- Virtual core device IDs (matching XIQueryDevice) ---
constexpr uint16_t kVirtualCorePointer  = 2;
constexpr uint16_t kVirtualCoreKeyboard = 3;

// --- Wire format sizes ---
// xXIDeviceEvent with buttons_len=1, valuators_len=0: 84 bytes
constexpr size_t kDeviceEventSize = 84;
constexpr uint32_t kDeviceEventLength = 13;  // (84 - 32) / 4

// xXIEnterEvent with buttons_len=1: 76 bytes
constexpr size_t kEnterEventSize = 76;
constexpr uint32_t kEnterEventLength = 11;  // (76 - 32) / 4

} // namespace x11::xi2
