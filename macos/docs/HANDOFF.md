# SwiftX11 — Session Handoff Prompt

**Date**: 2026-03-04
**Version**: v1.7.1
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

**Working clients**: xterm (scrollbar, cursor blink, modifier keys, Option+click thumb drag, bidirectional clipboard), xeyes, xcalc (with XFILESEARCHPATH auto-setup, Symbol font √÷π), xclock, multi-client (xterm + xeyes simultaneously)

**Advertised extensions**: BIG-REQUESTS (133), RENDER (139), XFIXES (134). SHAPE (135) has full stubs but is NOT advertised (breaks xeyes).

**Display**: SwiftX11 listens on display :1 (TCP port 6001) to avoid XQuartz conflict. `~/.profile` sets `DISPLAY=127.0.0.1:1`.

---

## What Was Accomplished (v1.7.0 → v1.7.1)

### v1.7.0: Phase 3 Font Infrastructure
- **PCF font support**: Full parser with TOC, metrics, bitmaps, encoding tables. MSB/LSB bit order handling. zlib decompression for .pcf.gz. Scans `/opt/X11/share/fonts/{misc,75dpi,100dpi}/`.
- **XLFD wildcard matching**: Iterative glob with `*` and `?`. Matches against PCF registry and builtin BDF fonts.
- **Font aliases**: System `fonts.alias` files loaded; alias targets resolved against PCF registry (99 aliases resolved).
- **Symbol font encoding**: Adobe Symbol font (symb12.pcf.gz, `adobe-fontspecific`) works — xcalc √÷π render correctly.
- **App sandbox disabled**: `ENABLE_APP_SANDBOX=NO` — sandbox blocked PCF font loading.
- **Categorical trace system**: `TraceDefs.hpp` with opt-in categories (RESIZE, PRESENT, LIFECYCLE, INPUT, RESOLVE, FONT).
- **ListFontsWithInfo**: Full implementation with per-font metrics + correct 60-byte terminator reply.
- **Scrollbar resize fix**: Second ExposeChildren after promoteDisplaySurface (150ms post-resize) ensures scrollbar redraws after xterm reconfigures children.

### v1.7.1: RENDER Triangles + Gradients
- **Triangles/TriStrip/TriFan (minor 11/12/13)**: Full scanline triangle rasterization. Sort vertices by Y, walk edges with FIXED 16.16 interpolation. Triangles = independent 3-vertex groups, TriStrip = sliding window, TriFan = shared pivot.
- **Gradient source pictures**: CreateLinearGradient (34), CreateRadialGradient (35), CreateConicalGradient (36) now create real gradient Pictures with per-pixel sampling. Linear: dot-product projection. Radial: concentric circle distance. Conical: atan2 angle. Color stop interpolation with Pad (clamp) mode.
- **Composite gradient support**: Composite (minor 8) now handles gradient sources alongside solid and drawable sources.
- All RENDER Phase 2.1 items are now complete.

---

## Known Issues (deferred)

### xcalc -rpn Extra Button Labels
**Symptom**: Buttons 40-54 show widget names. **NOT a server bug** — app-defaults only cover buttons 1-39.

### xclock/xcalc FontSet Warnings
**Symptom**: "Missing charsets in String to FontSet conversion" — XCreateFontSet() expects multiple charset fonts.

### Window close does not kill client
Closing the Cocoa window hides the NSWindow but the X11 client keeps running. Need WM_DELETE_WINDOW or socket close.

---

## Next Tasks (Priority Order)

### 1. Window Close → Client Kill
Red close button should terminate the X11 client. Two approaches:
- **WM_DELETE_WINDOW** (ICCCM-compliant): Check if client's WM_PROTOCOLS property includes WM_DELETE_WINDOW atom. If so, send a ClientMessage event with the WM_DELETE_WINDOW atom. Well-behaved clients (xterm, xcalc) will exit gracefully.
- **Forceful disconnect**: If client doesn't support WM_DELETE_WINDOW (or as fallback after timeout), close the client socket (fd) to force disconnect. Server's `eraseOwnedBy()` handles resource cleanup.
- Cmd+W should also trigger window close with the same behavior.
- Key files: `SwiftX11/UI/Windows/X11WindowHost.swift` (Cocoa windowWillClose), `X11LowLevel/cpp/X11Protocol/src/Ops/WindowOps.cpp` (DestroyWindow), `X11LowLevel/cpp/X11Protocol/include/Core/ClipboardAtoms.hpp` (WM_PROTOCOLS and WM_DELETE_WINDOW atom constants already defined)

### 2. Error Handling
Proper X11 error generation (BadWindow, BadDrawable, BadGC, etc.) with correct error reply format. Currently, requests referencing destroyed XIDs silently fail. Java/GTK toolkits may depend on error responses for resource management.

### 3. Enable SHAPE Extension
Implement actual shape-based pixel clipping, then advertise. Current stubs consume shape ops silently but don't clip.

### 4. Enable Remaining Extensions
Advertise RANDR, Xinerama, GE one at a time. Handler stubs already exist.

### 5. Container Networking
TCP + Unix socket + xauth for Docker workflow. Verify `DISPLAY=host.docker.internal:1` works from Docker container.

---

## Build & Test

```bash
# Open in Xcode
open macos/SwiftX11.xcodeproj

# Build target: SwiftX11 (macOS app)
# Test clients (in separate terminal):
xterm -sb -rightbar -bc    # scrollbar + cursor blink (DISPLAY set in ~/.profile)
xeyes                      # pointer tracking, multi-client
xcalc -rpn                 # calculator (XFILESEARCHPATH auto-set by SwiftX11)
xclock -analog             # Xaw widgets, arcs, timer events

# Verify version banner in console: "SwiftX11 v1.7.1"

# Test clipboard:
# 1. Select text in xterm → Cmd+V in macOS app (X11→macOS)
# 2. Copy text in macOS app → Option-click in xterm to paste (macOS→X11)
```

---

## Key Architecture References

Read these files to understand the codebase:
- `CLAUDE.md` — Comprehensive architecture doc (surface routing, damage pipeline, input events, development guidelines)
- `docs/TODO.md` — Full 5-phase roadmap with testing apps and priority order
- `X11LowLevel/cpp/X11Protocol/include/Core/XProtoModules.hpp` — All C++ protocol modules
- `X11LowLevel/cpp/X11Protocol/src/XProtoServerBridge.cpp` — Host command dispatch (button/focus/resize events)
- `X11LowLevel/cpp/X11Protocol/src/Ops/EventOps.cpp` — computeEventXYFromHostLocal, button/key event building
- `X11LowLevel/cpp/X11Protocol/src/Ops/ShapeOps.cpp` — PolyRectangle, PolyLine, PolyArc draw ops
- `X11LowLevel/cpp/X11Protocol/include/Core/GCTable.hpp` — GCState struct with all GC fields
- `X11LowLevel/cpp/X11Protocol/src/Ops/GCOps.cpp` — CreateGC/ChangeGC/CopyGC/SetDashes
- `X11LowLevel/cpp/X11Protocol/src/Ops/RenderOps.cpp` — RENDER extension (full: Composite, Trapezoids, Triangles, Glyphs, Gradients)
- `X11LowLevel/cpp/X11Protocol/src/Ops/ExtensionOps.cpp` — Extension stubs (XFIXES, SHAPE, RANDR, etc.)
- `X11LowLevel/cpp/X11Protocol/src/Ops/SelectionOps.cpp` — Selection protocol + clipboard bridge
- `X11LowLevel/cpp/X11Protocol/src/Ops/PropOps.cpp` — Property operations + ClipboardCapture
- `X11LowLevel/include/SwiftX11Version.h` — Single source of truth for version string

---

## Important Conventions

- **All new code in C++ or Swift** — no C files remain, no new C code
- **Display :1** — SwiftX11 uses TCP port 6001 (display :1), set via `~/.profile`
- **Version bump** — bump `SwiftX11Version.h` when making changes to verify correct build is running
- **Reply-bearing opcodes** — always send reply via `ctx.reply().sendReply32()` or XCB will crash
- **toX11State()** — always use for event state fields (modifier/button bit mapping)
- **stridePixels** — always use `dst.stridePixels` not `dst.w` for pixel row indexing
- **Branch**: all work on `develop++`, use `git -C /Users/lkg/Documents/Vivado/SwiftX11 merge <branch> --no-edit` to merge from worktree
- **Extension opcodes**: BIG-REQUESTS=133, XFIXES=134, SHAPE=135, RANDR=136, Xinerama=137, GE=138, RENDER=139 (defined in X11ExtOpcodes.hpp)
- **Do NOT advertise extensions** until core operations are complete — broken extension paths cause worse behavior than no extension
- **Opcode dispatch**: Modules register via `reg.registerMajor(opcode, &Class::onMajor, this)` in constructors
- **Wire helpers**: `wire::wr16_le`, `wire::wr32_le`, `wire::rd16_le`, `wire::rd32_le` for little-endian encoding
- **ByteReader**: `br.readU32()`, `br.readU16()`, `br.readU8()`, `br.readI16()`, `br.skip(n)`, `br.remaining()`
- **Trace tiers**: `#ifndef NDEBUG` for lifecycle traces always in debug; `X11_TRACE_<CATEGORY>` for categorical traces (RESIZE, PRESENT, LIFECYCLE, INPUT, RESOLVE, FONT); `X11_TRACE_VERBOSE` enables ALL categories. See `TraceDefs.hpp`.
