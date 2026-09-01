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

// ICCCM / WM protocol atoms (80+)
static constexpr uint32_t kWM_TAKE_FOCUS              = 80;

// EWMH atoms
static constexpr uint32_t k_NET_WM_WINDOW_TYPE        = 81;
static constexpr uint32_t k_NET_WM_WINDOW_TYPE_NORMAL = 82;
static constexpr uint32_t k_NET_WM_WINDOW_TYPE_DIALOG = 83;
static constexpr uint32_t k_NET_WM_WINDOW_TYPE_TOOLBAR= 84;
static constexpr uint32_t k_NET_WM_WINDOW_TYPE_UTILITY= 85;
static constexpr uint32_t k_NET_WM_WINDOW_TYPE_MENU   = 86;
static constexpr uint32_t k_NET_WM_WINDOW_TYPE_TOOLTIP= 87;
static constexpr uint32_t k_NET_WM_WINDOW_TYPE_SPLASH = 88;
static constexpr uint32_t k_NET_WM_STATE              = 89;
static constexpr uint32_t k_NET_WM_STATE_MODAL        = 90;
static constexpr uint32_t k_NET_WM_STATE_FULLSCREEN   = 91;
static constexpr uint32_t k_NET_FRAME_EXTENTS         = 92;

// Motif WM hints
static constexpr uint32_t k_MOTIF_WM_HINTS            = 93;

// Predefined atoms (1-68) commonly needed
static constexpr uint32_t kPRIMARY           = 1;
static constexpr uint32_t kSECONDARY         = 2;
static constexpr uint32_t kATOM              = 4;
static constexpr uint32_t kCARDINAL          = 6;
static constexpr uint32_t kSTRING            = 31;
static constexpr uint32_t kWM_HINTS          = 35;
static constexpr uint32_t kWM_NAME           = 39;
static constexpr uint32_t kWM_NORMAL_HINTS   = 40;

static constexpr uint32_t kFirstPreRegistered = 69;
static constexpr uint32_t kLastPreRegistered  = 93;

} // namespace atom
} // namespace x11
