# SwiftX11 — Session Handoff Prompt

**Date**: 2026-02-28
**Version**: v1.2.0
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

**Working clients**: xterm (with scrollbar, cursor blink, modifier keys, scrollbar thumb drag via Option+click), xeyes, xcalc (partially — no outlines, button routing broken), xclock (partially — FontSet warnings), multi-client (xterm + xeyes simultaneously)

**Display**: SwiftX11 listens on display :1 (TCP port 6001) to avoid XQuartz conflict. `~/.profile` sets `DISPLAY=127.0.0.1:1`.

---

## What Was Accomplished (v1.1.0 → v1.2.0)

### v1.1.1: Retained Display Buffer
- Pre-resize frame kept for flicker-free window resize
- Forces present after surface promotion to avoid white flash

### v1.2.0: Phase 1 Core Protocol Gaps
All Phase 1 opcodes implemented:

**Reply-bearing stubs** (crash prevention):
- QueryKeymap (44): Returns 32 zero bytes (no keys pressed)
- GetMotionEvents (39): Returns empty event list
- GetFontPath (52): Returns empty font path list

**Void stubs**:
- SetFontPath (51): Accepts and ignores
- ChangeActivePointerGrab (30): Updates active grab event mask via GrabTable
- DestroySubwindows (5): Destroys all descendants in depth-first order
- RotateProperties (114): Accepts and ignores

**GC value mask completion**:
- All 23 value mask bits (0-22) now parsed and stored in GCState
- New fields: line_width, line_style, cap_style, join_style, fill_style, fill_rule, tile, stipple, ts_x/y_origin, dash_offset, dashes_single, arc_mode, dash_list
- CopyGC extended to copy all new fields
- SetDashes (58): Stores dash offset and dash list

**ReparentWindow** (opcode 7):
- WindowTable::reparent() updates parent, x, y
- Handler unmaps if mapped, reparents, sends ReparentNotify event (type 21), remaps if was mapped

---

## Active Issues to Investigate

### 1. xcalc Missing Button Outlines (HIGH)
**Symptom**: Button borders/outlines not visible in xcalc.
**What we know**: PolyRectangle and PolyLine DO apply GC function via RasterOp.hpp. GC function and planemask are already implemented and used by draw ops.
**Likely cause**: GC color issue — possibly GXxor with fg≈bg producing invisible output, or a GC state not being set correctly for the outline drawing GC.
**Investigation approach**: Add trace logging in PolyRectangle/PolyLine to dump GC state (function, fg, bg, plane_mask) during xcalc rendering. Compare with what xcalc expects.

### 2. xcalc All Buttons Send "2" (HIGH)
**Symptom**: Pressing any button position in xcalc always sends the value "2".
**What we know**: Button events pick the deepest mapped child window under the pointer, then walk up to find a window with ButtonPress mask. The event's x,y coordinates are translated via `computeEventXYFromHostLocal()`.
**Likely cause**: Coordinate translation failing for xcalc's Xaw widget tree — all button presses resolve to the same child widget or the x,y coordinates map to the same button. Could be related to how Xaw internally positions button widgets vs. how we report event coordinates.
**Investigation approach**: Trace button events with `[BTN]` debug output, check which child XID is picked and what x,y coordinates are reported. Compare with the actual widget geometry in WindowTable.

### 3. xterm Uncleared Pixels at Bottom
**Symptom**: Occasional stale/uncleared pixels visible at bottom edge of xterm window.
**What we know**: This is intermittent. May be a damage rect boundary issue or ClearArea not covering the full area during resize/scroll.
**Investigation approach**: Check if it correlates with resize events or scroll operations. May be a 1-pixel-off issue in damage rect calculation.

### 4. xclock/xcalc FontSet Warnings
**Symptom**: "Missing charsets in String to FontSet conversion" / "Unable to load any usable fontset"
**What we know**: Phase 3 font infrastructure issue. Xlib's XCreateFontSet() calls ListFonts expecting multiple charset fonts; SwiftX11 has minimal BDF coverage (only fixed/cursor fonts).
**Not fixable in Phase 1** — would require bundling additional BDF/PCF fonts.

---

## Next Tasks (Priority Order)

### 1. Debug xcalc Issues
The xcalc button outline and button-value issues are the most pressing — they indicate potential problems that would affect Vivado too.

**For outlines**: Trace PolyRectangle during xcalc draw. Check GC state — is `function` correct? Is fg/bg set to values that would be visible? Is `mapPixelToARGB()` mapping correctly for xcalc's colors?

**For button routing**: Trace `[BTN]` events with xcalc running. Check:
- Which child XID is `pick_deepest_mapped_child` returning?
- What x,y coordinates are in the ButtonPress event?
- Are these coordinates in child-local space or host-local space?
- Does `computeEventXYFromHostLocal()` in EventOps.cpp correctly translate to child-local coords?

### 2. BIG-REQUESTS Extension
Vivado schematics exceed 256KB request limit. Implement:
- QueryExtension("BIG-REQUESTS") → present=1
- BigReqEnable → return max request size
- Wire format: length==0 → read 4-byte extended length

### 3. Error Handling
Proper X11 error generation (BadWindow, BadDrawable, etc.)

### 4. RENDER Extension
Anti-aliased fonts and alpha compositing for GTK/Java apps.

---

## Build & Test

```bash
# Open in Xcode
open macos/SwiftX11.xcodeproj

# Build target: SwiftX11 (macOS app)
# Test clients (in separate terminal):
xterm -sb -rightbar -bc    # scrollbar + cursor blink (DISPLAY set in ~/.profile)
xeyes                      # pointer tracking, multi-client
xcalc                      # button outlines + routing test
xclock -analog             # Xaw widgets, arcs, timer events

# Verify version banner in console: "SwiftX11 v1.2.0"
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
- `X11LowLevel/include/SwiftX11Version.h` — Single source of truth for version string

---

## Important Conventions

- **All new code in C++** — no C files remain, no new C code
- **Display :1** — SwiftX11 uses TCP port 6001 (display :1), set via `~/.profile`
- **Version bump** — bump `SwiftX11Version.h` when making changes to verify correct build is running
- **Reply-bearing opcodes** — always send reply via `ctx.reply().sendReply32()` or XCB will crash
- **toX11State()** — always use for event state fields (modifier/button bit mapping)
- **stridePixels** — always use `dst.stridePixels` not `dst.w` for pixel row indexing
- **Branch**: all work on `develop++`, use `git -C /Users/lkg/Documents/Vivado/SwiftX11 merge <branch> --no-edit` to merge from worktree
- **Opcode dispatch**: Modules register via `reg.registerMajor(opcode, &Class::onMajor, this)` in constructors
- **Wire helpers**: `wire::wr16_le`, `wire::wr32_le`, `wire::rd16_le`, `wire::rd32_le` for little-endian encoding
- **ByteReader**: `br.readU32()`, `br.readU16()`, `br.readU8()`, `br.readI16()`, `br.skip(n)`, `br.remaining()`
