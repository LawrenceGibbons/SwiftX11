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

// Raw event types and masks (global, typically on root window)
constexpr uint16_t kRawKeyPress       = 13;
constexpr uint16_t kRawKeyRelease     = 14;
constexpr uint16_t kRawButtonPress    = 15;
constexpr uint16_t kRawButtonRelease  = 16;
constexpr uint16_t kRawMotion         = 17;

constexpr uint32_t kRawKeyPressMask      = (1u << 13);
constexpr uint32_t kRawKeyReleaseMask    = (1u << 14);
constexpr uint32_t kRawButtonPressMask   = (1u << 15);
constexpr uint32_t kRawButtonReleaseMask = (1u << 16);
constexpr uint32_t kRawMotionMask        = (1u << 17);

// --- Virtual core device IDs (matching XIQueryDevice) ---
constexpr uint16_t kVirtualCorePointer  = 2;  // master pointer
constexpr uint16_t kVirtualCoreKeyboard = 3;  // master keyboard
constexpr uint16_t kXTESTPointer        = 4;  // slave pointer (sourceid for pointer events)
constexpr uint16_t kXTESTKeyboard       = 5;  // slave keyboard (sourceid for keyboard events)

// --- Wire format sizes (mirror xorg eventToDeviceEvent / xXIEnterEvent) ---
// xorg ALWAYS emits buttons_len=8 (MAX_BUTTONS=256 -> 32-byte mask) and, for
// device events, valuators_len=2 (MAX_VALUATORS=36 -> 8-byte mask) plus one
// FP3232 (8 bytes) per SET valuator.  A client that queries the device sees the
// valuator classes we advertise (x,y) and expects that axis data to be present
// in motion/button events; sending valuators_len=0 (our old layout) made
// Chromium/Electron register the axes then die on the first axis-less motion.
constexpr uint16_t kXIButtonsLen   = 8;   // bytes_to_int32(bits_to_bytes(256))
constexpr uint16_t kXIValuatorsLen = 2;   // bytes_to_int32(bits_to_bytes(36))

// Device event (motion/button) WITH x,y valuators:
//   80 (fixed header) + 32 (button mask) + 8 (valuator mask) + 16 (2 FP3232) = 136
constexpr size_t kDeviceEventSize = 136;
constexpr uint32_t kDeviceEventLength = 26;  // (136 - 32) / 4

// Key device event (no valuators set): 80 + 32 + 8 + 0 = 120
constexpr size_t kKeyEventSize = 120;
constexpr uint32_t kKeyEventLength = 22;     // (120 - 32) / 4

// xXIEnterEvent: 72 (fixed header) + 32 (button mask) = 104 (crossing has no valuators)
constexpr size_t kEnterEventSize = 104;
constexpr uint32_t kEnterEventLength = 18;  // (104 - 32) / 4

// xXIRawEvent with 2 valuators (X, Y axes):
// GenericEvent header (8) + evtype(2)+deviceid(2)+time(4)+detail(4)+
//   sourceid(2)+valuators_len(2)+flags(4)+pad(4) = 32 bytes header
// + valuator_mask[1] (4 bytes, bits 0+1 set)
// + raw_values[2] (2×FP32.32 = 16 bytes)
// + values[2] (2×FP32.32 = 16 bytes)
// Total: 68 bytes, length = (68-32)/4 = 9
constexpr size_t kRawEventSize = 68;
constexpr uint32_t kRawEventLength = 9;

} // namespace x11::xi2
