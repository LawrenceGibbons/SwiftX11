//
//  TraceDefs.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/28/26.
//
//  Categorical trace flags for SwiftX11 C++ protocol core.
//
//  Each flag gates a class of diagnostic fprintf() calls.
//  Enable individual categories by adding -DX11_TRACE_<CATEGORY>
//  to OTHER_CPLUSPLUSFLAGS in Xcode build settings.
//
//  X11_TRACE_VERBOSE enables ALL categories (existing behaviour).
//

#pragma once

// ---- Category defines ----

// Resize flow: SURFACE_UPDATE, SURFACE_RESIZED, applyRootlessResize,
//              ConfigureWindow child BG fill, damage during resize
#if defined(X11_TRACE_RESIZE) || defined(X11_TRACE_VERBOSE)
  #define X11_TRACE_RESIZE_ENABLED 1
#else
  #define X11_TRACE_RESIZE_ENABLED 0
#endif

// Present/damage: COPY_SURFACE, SNAPSHOT, DAMAGE, BG_FILL, BG_FILL_RETRY
#if defined(X11_TRACE_PRESENT) || defined(X11_TRACE_VERBOSE)
  #define X11_TRACE_PRESENT_ENABLED 1
#else
  #define X11_TRACE_PRESENT_ENABLED 0
#endif

// Window lifecycle: CreateWindow, MapWindow, MapSubwindows,
//                   SET_PRESENTABLE, EXPOSE_SUBTREE, EXPOSE_SEND
#if defined(X11_TRACE_LIFECYCLE) || defined(X11_TRACE_VERBOSE)
  #define X11_TRACE_LIFECYCLE_ENABLED 1
#else
  #define X11_TRACE_LIFECYCLE_ENABLED 0
#endif

// Input events: BTN, BTN_GRAB, FOCUS, DRAG_MOTION, CURSOR
#if defined(X11_TRACE_INPUT) || defined(X11_TRACE_VERBOSE)
  #define X11_TRACE_INPUT_ENABLED 1
#else
  #define X11_TRACE_INPUT_ENABLED 0
#endif

// Drawable resolve: RESOLVE success/fail details
#if defined(X11_TRACE_RESOLVE) || defined(X11_TRACE_VERBOSE)
  #define X11_TRACE_RESOLVE_ENABLED 1
#else
  #define X11_TRACE_RESOLVE_ENABLED 0
#endif
