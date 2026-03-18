//
//  X11ExtOpcodes.hpp
//  X11LowLevel
//
//  Assigned major opcodes and first_event / first_error for X11 extensions.
//

#pragma once
#include <cstdint>

namespace x11 {
namespace ext {

// Extension major opcodes (128–255 range, assigned by the server)
static constexpr uint8_t kBigReq    = 133;  // BIG-REQUESTS
static constexpr uint8_t kXFIXES    = 134;
static constexpr uint8_t kSHAPE     = 135;
static constexpr uint8_t kRANDR     = 136;
static constexpr uint8_t kXinerama  = 137;
static constexpr uint8_t kGE        = 138;  // Generic Event Extension
static constexpr uint8_t kRENDER    = 139;  // reserved for RENDER
static constexpr uint8_t kXCMisc   = 140;  // XC-MISC (XID range recycling)
static constexpr uint8_t kXInput2  = 141;  // XInput2 (XI2)
static constexpr uint8_t kXTEST    = 142;  // XTEST

// Extension first_event values
static constexpr uint8_t kXFIXES_FirstEvent   = 87;
static constexpr uint8_t kSHAPE_FirstEvent    = 76;
static constexpr uint8_t kRANDR_FirstEvent    = 89;
static constexpr uint8_t kGE_FirstEvent       = 35;  // GenericEvent

// XInput1 first_event: 17 events (DeviceValuator..DevicePropertyNotify)
// Range 93-109, safely above RANDR (89-90)
static constexpr uint8_t kXInput_FirstEvent  = 93;

// Extension first_error values (0 = none)
static constexpr uint8_t kSHAPE_FirstError    = 0;
static constexpr uint8_t kRANDR_FirstError    = 0;
static constexpr uint8_t kRENDER_FirstError   = 0;

} // namespace ext
} // namespace x11
