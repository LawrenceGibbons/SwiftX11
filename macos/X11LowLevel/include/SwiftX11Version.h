//
//  SwiftX11Version.h
//  X11LowLevel
//
//  Single source of truth for the SwiftX11 version string.
//  Consumed by Swift (via bridging header), C++ (via #include), and
//  eventually by About dialogs and other UI surfaces.
//

#pragma once

#define SWIFTX11_VERSION_BASE "1.19.36"

// Bump this for each diagnostic/debug iteration within a version.
// Set to 0 for release builds. Non-zero appends ".N-dbg" to version.
#define SWIFTX11_DEBUG_BUILD 27

#if SWIFTX11_DEBUG_BUILD
  #define SWIFTX11_VERSION SWIFTX11_VERSION_BASE "." _SWIFTX11_STRINGIFY(SWIFTX11_DEBUG_BUILD) "-dbg"
  #define _SWIFTX11_STRINGIFY(x) _SWIFTX11_STRINGIFY2(x)
  #define _SWIFTX11_STRINGIFY2(x) #x
#else
  #define SWIFTX11_VERSION SWIFTX11_VERSION_BASE
#endif
