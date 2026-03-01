# SwiftX11 — Session Handoff Prompt

**Date**: 2026-03-01
**Version**: v1.4.0
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

## What Was Accomplished (v1.2.0 → v1.4.0)

### v1.3.0: Borders + Button Routing Fixes
- GC function (GXxor, etc.) and planemask enforcement in PolyFillArc, PolyArc, FillPoly, PolyLine, PolySegment, PolyPoint
- WarpPointer (opcode 41) via UICommandQueue → Swift → CGWarpMouseCursorPosition

### v1.4.0: Phase 2 Extensions + Remaining Phase 1

**BIG-REQUESTS extension** (full implementation):
- QueryExtension("BIG-REQUESTS") returns present=1, major opcode 133
- BigReqEnable (opcode 133) sets per-client flag, replies with max_request_length=1M words (4MB)
- Wire format: length==0 in header → read 4-byte extended length (phase 0.5 in readAndDispatch)
- Per-client `big_req_enabled_` flag on XClient

**16-bit text**:
- PolyText16 (opcode 75): CHAR2B encoding (byte1 high, byte2 low), TEXTITEM16 elements
- ImageText16 (opcode 77): CHAR2B encoding, background rect fill + foreground glyphs

**Extension handler code** (version queries + basic ops — **NOT advertised to clients yet**):
- XFIXES (major 134): QueryVersion → 5.0
- SHAPE (major 135): QueryVersion → 1.1
- RANDR (major 136): QueryVersion → 1.5
- Xinerama (major 137): QueryVersion → 1.1, IsActive → true, QueryScreens → 1 screen 1920x1080
- GE (major 138): QueryVersion → 1.0
- Handler code registered in dispatch table; QueryExtension returns present=0 until ops are complete

**RENDER extension** (major 139 — handler code exists, **NOT advertised to clients yet**):
- QueryVersion → 0.11
- QueryPictFormats: ARGB32, RGB24, A8, A4, A1 formats + screen/visual mapping
- CreatePicture/ChangePicture/FreePicture: picture table management
- Composite: PictOpSrc, PictOpOver, PictOpAdd, PictOpClear (solid-fill + drawable-to-drawable)
- FillRectangles: solid color fill with compositing ops
- CreateSolidFill: solid color pictures
- QueryFilters: "nearest" and "bilinear"
- Glyph stubs: CreateGlyphSet/FreeGlyphSet/ReferenceGlyphSet/AddGlyphs/FreeGlyphs/CompositeGlyphs (consume silently)
- Transform/Filter/Gradient stubs: consume silently
- **Missing**: Trapezoids/Triangles, CompositeGlyphs rendering, Composite mask parameter

**Selections/clipboard**: Already implemented (SetSelectionOwner, GetSelectionOwner, ConvertSelection, SendEvent)

**Critical lesson learned**: Do NOT advertise extensions via QueryExtension until core operations are complete. Advertising SHAPE caused xeyes to use shape clipping (broken). Advertising RENDER caused xeyes to use Composite (broken — missing Trapezoids). Extensions must be enabled one at a time with testing.

---

## Known Issues (deferred)

### xcalc Missing Button Outlines (Phase 3)
**Symptom**: Button borders/outlines not visible in xcalc.
**Likely cause**: GC color issue — GXxor with fg~bg producing invisible output, or GC state not set correctly for outline drawing GC.
**Deferred**: User confirmed xcalc is acceptable until Phase 3.

### xcalc All Buttons Send "2" (Phase 3)
**Symptom**: Pressing any button position in xcalc always sends the value "2".
**Likely cause**: Coordinate translation failing for xcalc's Xaw widget tree.
**Deferred**: User confirmed xcalc is acceptable until Phase 3.

### xterm Uncleared Pixels at Bottom (LOW)
**Symptom**: Occasional stale/uncleared pixels visible at bottom edge of xterm window.

### xclock/xcalc FontSet Warnings (Phase 3)
**Symptom**: "Missing charsets in String to FontSet conversion"
Requires additional BDF/PCF font bundling.

---

## Next Tasks (Priority Order)

### 1. Window Close → Client Kill
Red close button should terminate the X11 client (WM_DELETE_WINDOW or socket close). Cmd+W should also work.

### 2. Error Handling
Proper X11 error generation (BadWindow, BadDrawable, BadGC, etc.) with correct error reply format.

### 3. Enable RENDER Extension
Complete missing operations (Trapezoids, CompositeGlyphs rendering, Composite mask), then advertise RENDER=present. Test with `rendercheck` and xeyes.

### 4. Enable Remaining Extensions
Complete and advertise one at a time: SHAPE (ShapeRectangles/ShapeMask), XFIXES (cursor visibility), RANDR, Xinerama, GE. Test each with xeyes/xterm before advertising the next.

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

# Verify version banner in console: "SwiftX11 v1.4.0"
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
- `X11LowLevel/cpp/X11Protocol/src/Ops/RenderOps.cpp` — RENDER extension implementation
- `X11LowLevel/cpp/X11Protocol/src/Ops/ExtensionOps.cpp` — Extension stubs (XFIXES, SHAPE, RANDR, etc.)
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
- **Extension opcodes**: BIG-REQUESTS=133, XFIXES=134, SHAPE=135, RANDR=136, Xinerama=137, GE=138, RENDER=139 (defined in X11ExtOpcodes.hpp)
- **Opcode dispatch**: Modules register via `reg.registerMajor(opcode, &Class::onMajor, this)` in constructors
- **Wire helpers**: `wire::wr16_le`, `wire::wr32_le`, `wire::rd16_le`, `wire::rd32_le` for little-endian encoding
- **ByteReader**: `br.readU32()`, `br.readU16()`, `br.readU8()`, `br.readI16()`, `br.skip(n)`, `br.remaining()`
