# SwiftX11 — Session Handoff Prompt

**Date**: 2026-02-28
**Version**: v1.0.3
**Branch**: `develop++`

---

## How to Use This Document

Paste this into a new Claude conversation along with: "Please read CLAUDE.md and docs/TODO.md, then continue with the next task outlined in this handoff."

---

## Project Summary

SwiftX11 is an X11 protocol server running natively on macOS. It implements the X11 wire protocol so X11 clients (xterm, xeyes, etc.) display via native Cocoa/Metal windows.

**Two language layers** (C layer fully eliminated in v1.0.0):
- **Swift** (`SwiftX11/`): UI owner — AppKit, Metal/software rendering, surface allocation, networking
- **C++** (`X11LowLevel/cpp/X11Protocol/`): Protocol core — request parsing, reply/event framing, raster ops, resource tables. `SwiftBridge.cpp` provides the `extern "C"` functions that Swift calls.

**Working clients**: xterm (with scrollbar, cursor blink, modifier keys, scrollbar thumb drag via Option+click), xeyes, multi-client (xterm + xeyes simultaneously)

**Display**: SwiftX11 listens on display :1 (TCP port 6001) to avoid XQuartz conflict. `~/.profile` sets `DISPLAY=127.0.0.1:1`.

---

## What Was Accomplished (v0.7.0 → v1.0.3)

### v1.0.0: C Layer Elimination
- Deleted x11_shim.c (926 lines), x11_backend.c (747 lines), x11_requests.c (~380 lines), x11_xproto.c (538 lines) and 8 associated headers (~2,600 lines total)
- Created SwiftBridge.cpp (extern "C" pass-throughs to C++ bridge functions)
- Created X11Setup.cpp/hpp (setup handshake moved from x11_xproto.c)
- UICommandQueue::push() calls x11_ui_push_*() directly (no C request queue, no C runloop thread)
- Architecture simplified to: Swift <-> SwiftBridge.cpp (extern "C") <-> C++ classes

### v1.0.1: Modifier Key Fix
- `sendKeyEvent()` was using raw `buttons | mods` instead of `toX11State()`, causing Option→Control and Control→Shift mapping errors
- Fixed: all event builders now use `toX11State()` consistently

### v1.0.2: XCB Sequence Crash + Cursor Blink
- Re-enabled GrabPointer reply (opcode 26) — missing reply caused XCB sequence desync crash during scrollbar use
- Changed `mapWindow()` from `orderFront(nil)` to `makeKeyAndOrderFront(nil)` so new windows receive focus and cursor blink starts immediately

### v1.0.3: Focus Guard
- Stale FocusOut from destroyed non-focused windows no longer steals focus
- When xeyes killed while xterm has focus, xterm cursor keeps blinking (previously stopped)
- FocusOut path guarded by `focus_host == host` check in XProtoServerBridge.cpp

### Documentation
- Complete rewrite of TODO.md with 5-phase Vivado/Vitis roadmap
- Testing applications guide per phase (xdpyinfo, xev, xclock, rendercheck, gtk3-demo, etc.)
- CLAUDE.md updated to reflect v1.0.3 state

---

## Next Tasks (Vivado/Vitis Roadmap)

The user's goal is full support for Xilinx Vivado and Vitis Linux tools running from a Docker container. See `docs/TODO.md` for the comprehensive plan. Priority order:

### 1. GC Clipping (SetClipRectangles) — HIGHEST PRIORITY
Without GC clip regions, drawing bleeds outside widget bounds. Every toolkit (Java Swing, GTK, Xaw) sets clip rectangles.
- Implement opcode 59 (SetClipRectangles)
- Store clip rect in GC
- Enforce clip in all draw ops (PolyFillRectangle, CopyArea, PutImage, text ops, etc.)

### 2. Missing Reply-Bearing Opcodes — CRASH PREVENTION
Unhandled reply-bearing opcodes cause XCB sequence desync:
- QueryKeymap (opcode 44): Return all-zeros stub
- GetMotionEvents (opcode 39): Return empty list stub
- GetFontPath (opcode 52): Return empty font path list

### 3. ReparentWindow (opcode 7)
GTK reparents widgets internally. Must update parent chain in WindowTable and adjust geometry.

### 4. BIG-REQUESTS Extension
Vivado schematics exceed 256KB request limit. Implement BigReqEnable + extended-length wire format.

### 5. RENDER Extension (Major Feature)
Anti-aliased fonts and alpha compositing. Java 2D, GTK/Cairo, and Pango all use RENDER.

### Testing Apps for Phase 1
Try these to validate progress:
```bash
DISPLAY=127.0.0.1:1 xdpyinfo          # display enumeration
DISPLAY=127.0.0.1:1 xev               # event diagnostics
DISPLAY=127.0.0.1:1 xclock -analog    # Xaw widgets, GC clipping
DISPLAY=127.0.0.1:1 xcalc             # complex widget tree
```

---

## Build & Test

```bash
# Open in Xcode
open macos/SwiftX11.xcodeproj

# Build target: SwiftX11 (macOS app)
# Test clients (in separate terminal):
xterm -sb -rightbar -bc    # scrollbar + cursor blink (DISPLAY set in ~/.profile)
xeyes                      # pointer tracking, multi-client

# Verify version banner in console: "SwiftX11 v1.0.3"
# Check for unhandled opcodes: grep "[DISPATCH] UNHANDLED" in console
```

---

## Key Architecture References

Read these files to understand the codebase:
- `CLAUDE.md` — Comprehensive architecture doc (surface routing, damage pipeline, input events, development guidelines)
- `docs/TODO.md` — Full 5-phase roadmap with testing apps and priority order
- `X11LowLevel/cpp/X11Protocol/include/Core/XProtoModules.hpp` — All C++ protocol modules
- `X11LowLevel/cpp/X11Protocol/src/XProtoServerBridge.cpp` — Host command dispatch
- `X11LowLevel/cpp/X11Protocol/src/Transport/XProtoDaemon.cpp` — Client connection lifecycle, poll loop
- `X11LowLevel/include/SwiftX11Bridge.h` — The C bridge API that Swift calls
- `X11LowLevel/cpp/X11Protocol/src/SwiftBridge.cpp` — C++ implementations of SwiftX11Bridge.h functions

---

## Important Conventions

- **All new code in C++** — no C files remain, no new C code
- **Display :1** — SwiftX11 uses TCP port 6001 (display :1), set via `~/.profile`
- **Version bump** — bump `SwiftX11Version.h` when making changes to verify correct build is running
- **Reply-bearing opcodes** — always send reply via `ctx.reply().sendReply32()` or XCB will crash
- **toX11State()** — always use for event state fields (modifier/button bit mapping)
- **stridePixels** — always use `dst.stridePixels` not `dst.w` for pixel row indexing
- **Branch**: all work on `develop++`
