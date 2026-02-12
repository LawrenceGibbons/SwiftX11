//
//  XEventMask.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#pragma once
#include <cstdint>

namespace x11::mask {

// Core SelectInput event mask bits (X11 protocol)
static constexpr uint32_t KeyPress          = 1u << 0;
static constexpr uint32_t KeyRelease        = 1u << 1;
static constexpr uint32_t ButtonPress       = 1u << 2;
static constexpr uint32_t ButtonRelease     = 1u << 3;
static constexpr uint32_t EnterWindow       = 1u << 4;
static constexpr uint32_t LeaveWindow       = 1u << 5;
static constexpr uint32_t PointerMotion     = 1u << 6;
static constexpr uint32_t PointerMotionHint = 1u << 7;
static constexpr uint32_t Button1Motion     = 1u << 8;
static constexpr uint32_t Button2Motion     = 1u << 9;
static constexpr uint32_t Button3Motion     = 1u << 10;
static constexpr uint32_t Button4Motion     = 1u << 11;
static constexpr uint32_t Button5Motion     = 1u << 12;
static constexpr uint32_t ButtonMotion      = 1u << 13;
static constexpr uint32_t KeymapState       = 1u << 14;
static constexpr uint32_t Exposure          = 1u << 15;
static constexpr uint32_t VisibilityChange  = 1u << 16;
static constexpr uint32_t StructureNotify   = 1u << 17;
static constexpr uint32_t ResizeRedirect    = 1u << 18;
static constexpr uint32_t SubstructureNotify= 1u << 19;
static constexpr uint32_t SubstructureRedirect=1u << 20;
static constexpr uint32_t FocusChange       = 1u << 21;
static constexpr uint32_t PropertyChange    = 1u << 22;
static constexpr uint32_t ColormapChange    = 1u << 23;
static constexpr uint32_t OwnerGrabButton   = 1u << 24;

} // namespace x11::mask
