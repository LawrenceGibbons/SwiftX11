//
//  ClipboardAtoms.hpp
//  X11LowLevel
//
//  Well-known atom IDs for clipboard/selection and WM protocol support.
//  These are pre-registered in AtomTable at startup (atoms 69+).
//

#pragma once
#include <cstdint>

namespace x11 {
namespace atom {

// Pre-registered dynamic atoms (69+)
static constexpr uint32_t kCLIPBOARD         = 69;
static constexpr uint32_t kTARGETS           = 70;
static constexpr uint32_t kUTF8_STRING       = 71;
static constexpr uint32_t kTIMESTAMP         = 72;
static constexpr uint32_t kTEXT              = 73;
static constexpr uint32_t kMULTIPLE          = 74;
static constexpr uint32_t kINCR              = 75;
static constexpr uint32_t kWM_PROTOCOLS      = 76;
static constexpr uint32_t kWM_DELETE_WINDOW  = 77;
static constexpr uint32_t kSWIFTX11_CLIP    = 78; // internal: proactive clipboard capture property
static constexpr uint32_t k_NET_WM_NAME     = 79;

// Predefined atoms (1-68) commonly needed
static constexpr uint32_t kPRIMARY           = 1;
static constexpr uint32_t kSECONDARY         = 2;
static constexpr uint32_t kATOM              = 4;
static constexpr uint32_t kSTRING            = 31;
static constexpr uint32_t kWM_NAME           = 39;

static constexpr uint32_t kFirstPreRegistered = 69;
static constexpr uint32_t kLastPreRegistered  = 79;

} // namespace atom
} // namespace x11
