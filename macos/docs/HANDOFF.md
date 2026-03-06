# SwiftX11 — Session Handoff Prompt

**Date**: 2026-03-06
**Version**: v1.8.3
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

**Display**: SwiftX11 listens on display :1 (TCP port 6001) to avoid XQuartz conflict. `~/.profile` sets `DISPLAY=127.0.0.1:1`.

---

## What Was Accomplished (v1.8.0 - v1.8.3)

### v1.8.0: CoreText Font Bridge (Phase 3.3)
- Maps X11 font requests (XLFD or bare names) to macOS system fonts via CoreText C API
- Family mapping: fixed->Menlo, courier->Courier, helvetica->Helvetica, times->Times New Roman, lucida->Lucida Grande
- Rasterizes Latin-1 glyphs (0-255) with both 1-bit bitmap and 8-bit alpha coverage
- `drawGlyphAlpha32` lambda in all 4 text handlers does per-pixel alpha blending
- Runtime toggle: Settings -> Rendering -> "Antialiased Fonts" (`std::atomic<bool> g_antialiased`)
- CoreText tried first in FontTable lookup; PCF/BDF fallback for cursor/symbol/fontspecific

### v1.8.2: Descender Clipping Fix
- Glyph positioning formula was `topY = y - bbx_yoff - (bbx_h - 1)` -- 1px too low vs X.org reference
- Fixed to `topY = y - bbx_yoff - bbx_h` at all 4 text handlers in `DrawOps.cpp`
- Also fixed CompositeGlyphs source picture resolution for old Xft 1x1 Repeat pixmap pattern

### v1.8.3: ARGB32 Glyph Parsing Fix
- `alphaBpp()` returned 8 for ARGB32 format (0x24) -- Xft uses ARGB32 for LCD/subpixel rendering (4 bytes/pixel)
- A8 parser read 1/4 of each glyph's bitmap, corrupting all subsequent glyphs in each AddGlyphs batch
- Fixed with 32bpp parsing path in `RenderOps.cpp`
- Added RENDER trace category to `TraceDefs.hpp`
- Gated all font/render debug traces behind categorical trace system

---

## Known Issues (deferred)

- **xcalc -rpn extra button labels**: Buttons 40-54 show widget names. NOT a server bug -- app-defaults only cover 1-39.
- **xclock/xcalc FontSet warnings**: "Missing charsets in String to FontSet conversion" -- XCreateFontSet() expects multiple charset fonts.
- **Window close does not kill client**: Closing Cocoa window hides NSWindow but X11 client keeps running.
- **xeyes shaped window occasional black flash on resize**: Minor cosmetic issue during live resize.

---

## Next Tasks (Priority Order)

### 1. Window Close -> Client Kill (Phase 5.4, HIGH)
Red close button should terminate the X11 client.
- **WM_DELETE_WINDOW** (ICCCM-compliant): Check WM_PROTOCOLS property for WM_DELETE_WINDOW atom, send ClientMessage. Atoms already pre-registered (68=WM_PROTOCOLS, 76=WM_DELETE_WINDOW in `ClipboardAtoms.hpp`).
- **Forceful disconnect**: Close client socket (fd). `eraseOwnedBy()` handles resource cleanup.
- **Cmd+W** should also trigger window close.
- **Files**: `X11WindowHost.swift` (windowWillClose), `SwiftBridge.cpp`, `XProtoServerBridge.cpp`, `EventOps.cpp`

### 2. Error Handling (Phase 4.1, HIGH)
No proper X11 errors generated -- requests referencing destroyed XIDs silently fail.
- **Need**: BadWindow, BadDrawable, BadGC, BadMatch, BadValue, BadAtom, BadPixmap, BadFont, BadAccess, BadAlloc
- **Format**: 32 bytes: type=0, error_code, seq, bad_value, minor_opcode, major_opcode
- **Files**: Every Ops/*.cpp file needs error checks; need new ErrorReply utility

### 3. Container Networking (Phase 5.2, HIGH)
TCP works on port 6001 but Unix sockets not implemented. Needed for Docker workflow.
- Unix socket: `/tmp/.X11-unix/X1`
- Xauth: Basic MIT-MAGIC-COOKIE-1 or xhost+ for development
- Verify `DISPLAY=host.docker.internal:1` from Docker

### 4. Phase 4 Remaining Items
- **Big-endian clients** (4.2): ByteReader/ReplyWriter assume little-endian. Java may connect big-endian.
- **Override-redirect** (4.3): Honor override_redirect window attribute.
- **Gravity** (4.3): win_gravity and bit_gravity for resize behavior.

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

# Verify version banner: "SwiftX11 v1.8.3"

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
- `X11LowLevel/cpp/X11Protocol/src/Ops/RenderOps.cpp` -- RENDER extension (Composite, Trapezoids, Triangles, Glyphs, Gradients)
- `X11LowLevel/cpp/X11Protocol/src/Fonts/CoreTextFont.cpp` -- CoreText font bridge
- `X11LowLevel/cpp/X11Protocol/src/Core/FontTable.cpp` -- Font table lookup chain

---

## Important Conventions

- **All new code in C++ or Swift** -- no C files remain
- **Display :1** -- TCP port 6001, set via `~/.profile`
- **Version bump** -- bump `SwiftX11Version.h` to verify correct build
- **Reply-bearing opcodes** -- always send reply via `ctx.reply().sendReply32()` or XCB will crash
- **toX11State()** -- always use for event state fields (modifier/button bit mapping)
- **stridePixels** -- always use `dst.stridePixels` not `dst.w` for pixel row indexing
- **Branch**: all work on `develop++`
- **Extension opcodes**: BIG-REQUESTS=133, XFIXES=134, SHAPE=135, RANDR=136, Xinerama=137, GE=138, RENDER=139 (in X11ExtOpcodes.hpp)
- **Trace tiers**: `#ifndef NDEBUG` for lifecycle traces; `X11_TRACE_<CATEGORY>` for categorical (RESIZE, PRESENT, LIFECYCLE, INPUT, RESOLVE, FONT, RENDER); `X11_TRACE_VERBOSE` enables ALL. See `TraceDefs.hpp`.
