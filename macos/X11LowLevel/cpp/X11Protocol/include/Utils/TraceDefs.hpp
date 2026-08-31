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

// ---- Opt-in categories (kept off by default in release builds) ----
//
// Historically v1.19.35.52-.61 enabled X11_TRACE_FONT / RENDER / DRAG
// unconditionally here while chasing live bugs.  The bugs are fixed
// (License Manager glyphs in v.53/.54, hw_ila drag in v.62), so these
// are now back to opt-in via -DX11_TRACE_<CATEGORY> in
// OTHER_CPLUSPLUSFLAGS, exactly like RESIZE / PRESENT / etc.  The
// names below are kept as a reminder of the available knobs:
//
//   -DX11_TRACE_FONT      — Font / glyph diagnostics
//   -DX11_TRACE_RENDER    — RENDER extension diagnostics
//   -DX11_TRACE_DRAG      — Drag session bracketing + motion routing
//   -DX11_TRACE_VERBOSE   — every category at once

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
//
// TEMPORARILY force-enabled (v1.19.36.4-dbg, Aug 2026) for the vlm
// scrollbar investigation — need visibility into backbuffer (pixmap-dst)
// rendering.  Revert to opt-in once the vlm scroll bugs are fixed.
#define X11_TRACE_RENDER 1
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

// Drag tracing: button-down begins a [DRAG] session, each motion
// during the drag bumps a counter, button-up emits a summary line.
// Used to diagnose Java AWT / Vivado hw_ila_x drag-and-drop failures.
#if defined(X11_TRACE_DRAG) || defined(X11_TRACE_VERBOSE)
  #define X11_TRACE_DRAG_ENABLED 1
#else
  #define X11_TRACE_DRAG_ENABLED 0
#endif
