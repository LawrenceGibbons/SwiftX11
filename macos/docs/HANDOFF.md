# SwiftX11 — Session Handoff Prompt

**Date**: 2026-03-09
**Version**: v1.10.0
**Branch**: `develop++`

---

## How to Use This Document

Paste this into a new Claude conversation along with: "Please read CLAUDE.md and docs/TODO.md, then continue with the next task outlined in this handoff."

---

## Project Summary

SwiftX11 is an X11 protocol server running natively on macOS. It implements the X11 wire protocol so X11 clients (xterm, xeyes, xcalc, xclock, etc.) display via native Cocoa/Metal windows.

**Two language layers** (C layer fully eliminated in v1.0.0):
- **Swift** (`SwiftX11/`): UI owner — AppKit, Metal/software rendering, surface allocation, networking
- **C++** (`X11LowLevel/cpp/X11Protocol/`): Protocol core — request parsing, reply/event framing, raster ops, resource tables. `SwiftBridge.cpp` provides the `extern "C"` functions that Swift calls.

**Working clients**: xterm (scrollbar, cursor blink, modifier keys, Option+click thumb drag, bidirectional clipboard, CoreText antialiased fonts, Xft subpixel rendering), xeyes (SHAPE transparency), xcalc (Symbol font sqrt/pi/division), xclock, multi-client

**Advertised extensions (7)**: BIG-REQUESTS (133), RENDER (139), XFIXES (134), SHAPE (135), RANDR (136), XINERAMA (137), GE (138)

**Display**: SwiftX11 listens on display :1 (TCP port 6001 + Unix socket `/tmp/.X11-unix/X1`). `~/.profile` sets `DISPLAY=127.0.0.1:1`.

---

## What Was Accomplished Recently (v1.9.3 - v1.10.0)

### v1.10.0: Rendering Performance + Metal-Only
- **Metal rendering required**: Software (CGImage/CALayer) rendering path removed entirely. SwiftX11 now requires a Metal-capable GPU (all Macs since 2012). Removed ~200 lines of software fallback code, `setupSoftwareLayer()`, `presentSoftware()`, `presentSoftwarePartial()`, `makeCGImage()`, `imageLayer`, `usingMetal`/`wantsMetal` flags, and the "Use Metal Rendering" settings toggle. `setUseMetal()` replaced with `ensureMetalSetup()` (lazy one-time init).
- **PutImage bulk memcpy**: GXcopy fast path now uses `std::memcpy` for the full row, then 4-pixel-unrolled alpha forcing (`dp[i] |= 0xFF000000u`), instead of per-pixel copy-and-OR. System memcpy is SIMD-optimized, yielding significant speedup for large PutImage calls (Vivado waveform/schematic renders).
- **Expose two-pass optimization**: `sendExposeSubtree()` and `ExposeChildren` handler restructured to fill all backgrounds/borders first, then send all Expose events. Ensures children have correct visual state before clients begin redrawing.

### v1.9.7: Multi-Monitor Support
- ScreenLayout cache, dynamic RANDR/Xinerama, coordinate conversion fixes
- `xrandr --query` works with real monitor configuration

### v1.9.6: UX Bug Fixes
- Ctrl+click → button 3, window persistence fix, QueryTree root children

### v1.9.5: Window Management Attributes
- override_redirect, win_gravity, bit_gravity, backing_store

### v1.9.4: Container/Network Support
- TCP + Unix domain socket dual listeners

### v1.9.3: X11 Error Handling
- Proper error generation across ~50 request handlers

---

## Known Issues (deferred)

- **xcalc -rpn extra button labels**: Buttons 40-54 show widget names. NOT a server bug -- app-defaults only cover 1-39.
- **xclock/xcalc FontSet warnings**: "Missing charsets in String to FontSet conversion" -- XCreateFontSet() expects multiple charset fonts.
- **xeyes shaped window occasional black flash on resize**: Minor cosmetic issue during live resize.
- **xrandr gamma warning**: `Failed to get size of gamma for output Virtual-0` -- RRGetCrtcGammaSize returns 0. Low priority cosmetic.

---

## Next Task: See docs/TODO.md

Check `docs/TODO.md` for remaining Phase 5 items (Xauth, keyboard mapping, window menu) and Phase 7 (additional extensions). Priority order from TODO.md:
- Phase 5.2: Xauth + latency tolerance (for Docker security)
- Phase 5.3: Full keyboard/keysym mapping
- Phase 5.5: Window menu integration
- Phase 7: XInput2, SYNC (if needed by specific apps)

---

## Build & Test

```bash
# Open in Xcode
open macos/SwiftX11.xcodeproj

# Build target: SwiftX11 (macOS app)
# Test clients:
xterm -sb -rightbar -bc        # scrollbar + cursor blink
xterm -fa Menlo -fs 16         # Xft antialiased rendering
xeyes                          # SHAPE transparency
xcalc                          # Symbol font (sqrt, pi, division)
xclock -analog                 # Xaw widgets, arcs
xrandr --query                 # Multi-monitor: shows real monitor config

# Verify version banner: "SwiftX11 v1.10.0"

# Test window close:
# 1. Launch xterm, click red close button -> xterm process should exit
# 2. Launch xterm, Cmd+W -> xterm process should exit
# 3. Launch xeyes, click red close button -> xeyes should exit (forceful disconnect)

# Test clipboard:
# 1. Select text in xterm -> Cmd+V in macOS app (X11->macOS)
# 2. Copy in macOS -> middle-click in xterm to paste (macOS->X11)
```

---

## Key Architecture References

Read these files to understand the codebase:
- `CLAUDE.md` -- Comprehensive architecture doc (surface routing, damage pipeline, input events, development guidelines)
- `docs/TODO.md` -- Full roadmap with testing apps and priority order
- `X11LowLevel/include/SwiftX11Version.h` -- Single source of truth for version string
- `X11LowLevel/cpp/X11Protocol/include/Utils/TraceDefs.hpp` -- Categorical trace system (RESIZE, PRESENT, LIFECYCLE, INPUT, RESOLVE, FONT, RENDER)
- `X11LowLevel/cpp/X11Protocol/include/Core/ScreenLayout.hpp` -- Multi-monitor layout cache
- `X11LowLevel/cpp/X11Protocol/src/Ops/ExtensionOps.cpp` -- RANDR/Xinerama/SHAPE/XFIXES handlers
- `X11LowLevel/cpp/X11Protocol/src/Ops/RenderOps.cpp` -- RENDER extension (Composite, Trapezoids, Triangles, Glyphs, Gradients)
- `X11LowLevel/cpp/X11Protocol/src/Fonts/CoreTextFont.cpp` -- CoreText font bridge

---

## Important Conventions

- **All new code in C++ or Swift** -- no C files remain
- **Display :1** -- TCP port 6001 + Unix socket, set via `~/.profile`
- **Version bump** -- bump `SwiftX11Version.h` to verify correct build
- **Reply-bearing opcodes** -- always send reply via `ctx.reply().sendReply32()` or XCB will crash
- **toX11State()** -- always use for event state fields (modifier/button bit mapping)
- **stridePixels** -- always use `dst.stridePixels` not `dst.w` for pixel row indexing
- **Branch**: all work on `develop++`
- **Extension opcodes**: BIG-REQUESTS=133, XFIXES=134, SHAPE=135, RANDR=136, Xinerama=137, GE=138, RENDER=139 (in X11ExtOpcodes.hpp)
- **Trace tiers**: `#ifndef NDEBUG` for lifecycle traces; `X11_TRACE_<CATEGORY>` for categorical (RESIZE, PRESENT, LIFECYCLE, INPUT, RESOLVE, FONT, RENDER); `X11_TRACE_VERBOSE` enables ALL. See `TraceDefs.hpp`.
