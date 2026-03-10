# SwiftX11 — Session Handoff Prompt

**Date**: 2026-03-09
**Version**: v1.9.7
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

## What Was Accomplished Recently (v1.9.3 - v1.9.7)

### v1.9.7: Multi-Monitor Support
- **ScreenLayout cache** (`ScreenLayout.hpp/cpp`): Queries `CGGetActiveDisplayList` for all monitors, caches position/size/pixel-size/physical-mm/name per display. Thread-safe with `std::mutex`. Auto-refreshes via `CGDisplayRegisterReconfigurationCallback`.
- **Dynamic RANDR**: `RRGetScreenResources` returns N outputs/CRTCs/modes from real monitor data. `RRGetOutputInfo`, `RRGetCrtcInfo`, `RRGetOutputPrimary` all use ScreenLayout. Added handlers for minors 27 (GetCrtcTransform), 28 (GetPanning), 29 (SetPanning), 32 (GetProviders).
- **Dynamic Xinerama**: `XineramaQueryScreens` returns real per-monitor rectangles.
- **X11 connection setup**: Virtual desktop size from ScreenLayout (union of all monitors).
- **GetGeometry on root**: Returns actual virtual desktop dimensions (was hardcoded 800x600).
- **Coordinate conversion fixes**: Four locations in Swift that used `NSScreen.main` now use virtual desktop bounds (`screens.map { $0.frame.minX }.min()`, `.maxY.max()`). New `x11RootToMacOSOrigin()` helper.
- **WarpPointer fix**: Uses `vmaxY` from all screens instead of `NSScreen.main?.frame.height`.
- **OR window Metal drawable retry**: Popup menus on secondary monitors had blank rendering due to Metal drawable timing race. Fixed with retry mechanism + `setFrame(display: true)`.
- **Removed stale `kRootW/kRootH`** constants (were hardcoded 800x600).
- `xrandr --query` now works, showing real monitor configuration.

### v1.9.6: UX Bug Fixes
- Ctrl+click correctly maps to X11 right-click (button 3)
- Window persistence fix (windowDidChangeOcclusionState no longer unmaps)
- QueryTree root children fix (xwininfo -root -tree works)

### v1.9.5: Window Management Attributes
- override_redirect, win_gravity, bit_gravity, backing_store fully parsed and reported
- Override-redirect windows create borderless floating NSWindows

### v1.9.4: Container/Network Support
- TCP + Unix domain socket dual listeners
- Docker workflow: `DISPLAY=host.docker.internal:1`

### v1.9.3: X11 Error Handling
- Proper error generation across ~50 request handlers in 14 files
- BadWindow, BadDrawable, BadPixmap, BadFont, BadAtom, BadColor, BadValue

---

## Known Issues (deferred)

- **xcalc -rpn extra button labels**: Buttons 40-54 show widget names. NOT a server bug -- app-defaults only cover 1-39.
- **xclock/xcalc FontSet warnings**: "Missing charsets in String to FontSet conversion" -- XCreateFontSet() expects multiple charset fonts.
- **xeyes shaped window occasional black flash on resize**: Minor cosmetic issue during live resize.
- **xrandr gamma warning**: `Failed to get size of gamma for output Virtual-0` -- RRGetCrtcGammaSize returns 0. Low priority cosmetic.
- **Multi-monitor popup testing pending**: OR window Metal drawable retry fix committed but untested with dual monitors (user has only laptop currently).

---

## Next Task: Phase 5.1 — Rendering Performance (MEDIUM priority)

This is what the user explicitly chose as the next priority. Three items from TODO.md:

### 1. Software Present Path: Partial CGImage Blit
**Current**: `presentSoftware()` in `X11WindowHost.swift` creates a full CGImage from the entire host surface every frame, even when only a small region changed. Metal path already does partial texture uploads via `MTLTexture.replace(region:)`.
**Goal**: Use `CGImage(cropping:)` or sub-image creation to only upload the damaged region in the software fallback path.
**Key files**: `SwiftX11/UI/Windows/X11WindowHost.swift` (presentBGRA/presentSoftware methods)

### 2. PutImage Optimization
**Current**: Large PutImage calls (e.g., full-window background images) go through per-pixel processing with alpha forcing and GC function application even for the common GXcopy case.
**Goal**: Fast memcpy path for GXcopy + no-clip + full-alpha cases; SIMD for alpha forcing in bulk.
**Key files**: `X11LowLevel/cpp/X11Protocol/src/Ops/DrawOps.cpp` (PutImage handler)

### 3. Expose Coalescing
**Current**: Each child window gets its own Expose event. Window resize can trigger many individual Expose events.
**Goal**: Batch expose events where possible to reduce client redraw overhead.
**Key files**: `X11LowLevel/cpp/X11Protocol/src/Ops/WindowOps.cpp` (sendExposeSubtree, ExposeChildren)

### Getting Started
Read these files first to understand the present pipeline:
- `SwiftX11/UI/Windows/X11WindowHost.swift` — `presentBGRA()`, `presentSoftware()`, `snapshotAndPresentNow()`
- `SwiftX11/Core/WindowRegistry.swift` — `schedulePresent()`, present timer
- `X11LowLevel/cpp/X11Protocol/src/UI/UICommandQueue.cpp` — shared damage accumulator (`x11_shared_damage_union/consume`)
- `SwiftX11/UI/Windows/X11MetalRenderer.swift` — Metal partial texture upload (reference for what software path should do)
- `X11LowLevel/cpp/X11Protocol/src/Ops/DrawOps.cpp` — PutImage handler

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

# Verify version banner: "SwiftX11 v1.9.7"

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

## Recent Commits on develop++

```
4d47203 Bump version to v1.9.7, update docs for multi-monitor support
c1658fd Add RRGetCrtcTransform (minor 27) handler — identity transform reply
6c5f5c2 Fix blank OR windows on secondary monitors (Metal drawable retry)
8ee7cf4 Add RRGetPanning (minor 28) and RRSetPanning (minor 29) handlers
4ba17a2 Fix RRGetOutputInfo 36-byte reply + RANDR debug traces
3b76f66 Fix RANDR minor opcodes + add RRGetProviders + present diagnostic traces
b9b37e1 Remove dead x11_get_virtual_desktop_px() from X11Setup.cpp
4754071 Merge multi-monitor support (ScreenLayout, RANDR, Xinerama, coordinate fixes)
```

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
