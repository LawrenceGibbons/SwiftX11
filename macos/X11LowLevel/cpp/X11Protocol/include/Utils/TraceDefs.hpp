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

// Font ops: OpenFont, ListFonts, ListFontsWithInfo, XLFD matching,
//           PCF loading, LABEL (text draw diagnostics)
#if defined(X11_TRACE_FONT) || defined(X11_TRACE_VERBOSE)
  #define X11_TRACE_FONT_ENABLED 1
#else
  #define X11_TRACE_FONT_ENABLED 0
#endif

// RENDER extension: CreatePicture, CreateGlyphSet, AddGlyphs,
//                   CompositeGlyphs, Composite, source promotion
#if defined(X11_TRACE_RENDER) || defined(X11_TRACE_VERBOSE)
  #define X11_TRACE_RENDER_ENABLED 1
#else
  #define X11_TRACE_RENDER_ENABLED 0
#endif

// Wire-level transport: every sendAll() 32-byte packet logged as
// [WIRE] ERROR/REPLY/EVENT.  Extremely noisy — hundreds of lines
// per client session.  Enable only when debugging sequence or
// reply-ordering issues.
#if defined(X11_TRACE_WIRE) || defined(X11_TRACE_VERBOSE)
  #define X11_TRACE_WIRE_ENABLED 1
#else
  #define X11_TRACE_WIRE_ENABLED 0
#endif

// Geometry tracing (window create/configure/resize/map/teardown):
// [GEOM] from CreateWindow, ConfigureWindow, applyRootlessResize,
// WM_HINTS_SET, WM_HINTS, FLUSH_MAP*, MAP_SHRUNK_BELOW_PEAK,
// POST_MAP_CONFIGNOTIFY, etc.  Added during the v1.19.35.40-49 popup
// size race + surface UAF investigation; extremely noisy during live
// resize (every windowDidResize tick fires multiple lines per host).
// Off by default in debug builds for normal client responsiveness.
// Enable with -DX11_TRACE_GEOM in OTHER_CPLUSPLUSFLAGS to diagnose
// further popup-size or surface-lifecycle issues.
#if defined(X11_TRACE_GEOM) || defined(X11_TRACE_VERBOSE)
  #define X11_TRACE_GEOM_ENABLED 1
#else
  #define X11_TRACE_GEOM_ENABLED 0
#endif
