# SwiftX11 — Session Handoff Prompt

**Date**: 2026-03-02
**Version**: v1.5.2
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

**Working clients**: xterm (with scrollbar, cursor blink, modifier keys, scrollbar thumb drag via Option+click, bidirectional clipboard), xeyes, xcalc (mostly — needs `XFILESEARCHPATH`, wrong Symbol font chars), xclock (partially — FontSet warnings), multi-client (xterm + xeyes simultaneously)

**Display**: SwiftX11 listens on display :1 (TCP port 6001) to avoid XQuartz conflict. `~/.profile` sets `DISPLAY=127.0.0.1:1`.

---

## What Was Accomplished (v1.5.0 → v1.5.2)

### v1.5.1: Bidirectional Clipboard Sync

**X11→macOS clipboard** (completed the other half of clipboard bridge):
- ClipboardCapture in PropOps detects UTF8_STRING/STRING property writes during selection transfer
- Pushes captured text to NSPasteboard via Swift `x11_clipboard_set()` callback
- Root-proxy selection owner: after capture, selection owner set to XID 1 (root window) so subsequent ConvertSelection from other X11 clients routes through server (serves from cached macOS clipboard)
- Sequence number stamping fix in `handleSendEvent` ensures correct SelectionNotify delivery
- Flow: Select text in xterm → xterm writes to property → ClipboardCapture → NSPasteboard → Cmd+V works in macOS apps
- Reverse: Copy in macOS → Option-click in xterm paste position → Xt sends ConvertSelection (owner=root) → server serves from NSPasteboard

### v1.5.2: CWBackPixmap/ParentRelative + xcalc Investigation

**CWBackPixmap (bit 0) handling**:
- CreateWindow: value 0 (None) = no change; value 1 (ParentRelative) = walk parent chain to inherit background
- ChangeWindowAttributes: same handling with additional None case to clear background
- `WindowTable::clearBackground(xid)` — sets `has_background_pixel=false` (server won't fill on ClearArea)
- `WindowTable::resolveParentRelativeBackground(xid)` — walks parent chain (max 64 depth), copies nearest ancestor's `background_pixel`

**xcalc -rpn investigation** (root cause found, partial workaround):
- Traced through all CreateWindow/ChangeWindowAttributes/ClearArea calls for xcalc — no CWBackPixel changes after creation, no ClearArea usage
- Root cause: xcalc's Xt app-defaults file (`/opt/X11/share/X11/app-defaults/XCalc`) was NOT being loaded — `XFILESEARCHPATH` env var not set
- **Workaround**: `XFILESEARCHPATH=/opt/X11/share/X11/%T/%N xcalc -rpn` loads app-defaults, fixing most button labels
- **Remaining issue**: xcalc creates 54 buttons in HP/RPN mode via `create_keypad()`, but app-defaults only define resources for buttons 1-39. Buttons 40-54 have no labels in app-defaults, so they show widget names ("button40", etc.)
- Buttons 21-22 have `mappedWhenManaged: False` in app-defaults (should be invisible in RPN mode) — Xt client-side concept
- Buttons 1, 10, 12 use Adobe Symbol font (`-adobe-symbol-*`) for special chars (sqrt, division, pi) — wrong characters is a font encoding issue

---

## xcalc Investigation Findings — Read Before Debugging xcalc

**IMPORTANT**: The previous session spent significant effort on a wrong hypothesis before finding the real cause. Save yourself time by reading this first.

### What Was Tried (and failed)
1. **Hypothesis: CWBackPixmap/ParentRelative causing white backgrounds**. We thought xcalc child widgets used CWBackPixmap=1 (ParentRelative), leaving `has_background_pixel=false`, so backgrounds stayed white and widget-name text was visible. We implemented ParentRelative support (which IS correct code for other apps). But **xcalc never uses ParentRelative** — 3 rounds of traces proved ALL xcalc windows are created with explicit CWBackPixel (bit0=0, bit1=1), bg_pixel=0xFFFFFFFF (white). Zero ClearArea calls, zero ChangeWindowAttributes with CWBackPixel.

### What the Real Problem Is
2. **Missing XFILESEARCHPATH**: Without `export XFILESEARCHPATH=/opt/X11/share/X11/%T/%N`, Xt can't find `/opt/X11/share/X11/app-defaults/XCalc`. Without this file, ALL 54 buttons display their widget names ("button1"..."button54") because Xaw Command widgets use the widget name as the default label. All backgrounds stay white because no color resources are loaded.

3. **54 buttons, not 39**: xcalc's `create_keypad()` creates 54 Command widgets (button1-button54) for HP/RPN mode. The app-defaults file (`/opt/X11/share/X11/app-defaults/XCalc`) only defines resources for buttons 1-39. Buttons 40-54 have **no label, no translation, no font** in the app-defaults, so even with XFILESEARCHPATH they show their widget names. This is an xcalc/app-defaults coverage gap, NOT a server bug.

4. **`mappedWhenManaged: False`**: Buttons 21 and 22 have this set in app-defaults (no labels defined for them either). This is a pure Xt client-side concept — Xt simply doesn't call XMapWindow for those widgets. If they appear, it's Xt behavior, not our server.

5. **Adobe Symbol font encoding**: Buttons 1 (sqrt: `\326\140`), 10 (division: `\270`), 12 (pi: `\160`) request `-adobe-symbol-*-*-*-*-*-120-*-*-*-*-*-*`. Our font table doesn't have this font, falls back to 9x15. Symbol encoding maps different codepoints than Latin-1 (e.g., octal 326 = 0xD6 = sqrt in Symbol but "O with diaeresis" in Latin-1). Fix is either bundle the Adobe Symbol BDF font or add an encoding translation layer.

### Diagnostic Technique Notes
- **Trace tier gotcha**: `X11_TRACE_LIFECYCLE_ENABLED` requires explicit `-DX11_TRACE_LIFECYCLE` compiler flag — it is NOT enabled in standard debug builds. Only `#ifndef NDEBUG` traces appear by default. See `TraceDefs.hpp` for all categories.
- **TrueColor pixel mapping**: WhitePixel=1, BlackPixel=0 with special-casing in `X11Setup.cpp`. Pixel 0→0xFF000000 (black), pixel 1→0xFFFFFFFF (white), others→0xFF000000|(val&0x00FFFFFF). This is technically wrong for TrueColor (WhitePixel should be 0x00FFFFFF) but works via the special-casing.

---

## Known Issues (deferred)

### xcalc -rpn Extra Button Labels
**Symptom**: 3+ buttons show default widget name labels even with app-defaults loaded.
**Root cause**: xcalc creates 54 buttons but app-defaults only cover 1-39. Buttons 40-54 show as default. Buttons 21-22 should be invisible (`mappedWhenManaged: False`).
**Workaround**: `XFILESEARCHPATH=/opt/X11/share/X11/%T/%N xcalc -rpn`
**NOT a server bug** — app-defaults coverage gap in the XCalc resource file.

### xcalc Wrong Symbol Characters (Phase 3)
**Symptom**: sqrt (button1), division (button10), pi (button12) display incorrectly.
**Root cause**: These buttons request `-adobe-symbol-*` font. Our BDF font system doesn't have Adobe Symbol; falls back to 9x15. Symbol encoding uses different codepoints than Latin-1.
**Fix needed**: Bundle Adobe Symbol BDF font or implement Symbol encoding mapping.

### xterm Uncleared Pixels at Bottom (LOW)
**Symptom**: Occasional stale/uncleared pixels visible at bottom edge of xterm window.

### xclock/xcalc FontSet Warnings (Phase 3)
**Symptom**: "Missing charsets in String to FontSet conversion"
Requires additional BDF/PCF font bundling.

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

### 3. Enable RENDER Extension
Complete missing operations (Trapezoids, CompositeGlyphs rendering, Composite mask), then advertise RENDER=present via QueryExtension. Test with `rendercheck` and xeyes. This is the single most impactful extension for modern toolkit support.

### 4. Enable Remaining Extensions
Complete and advertise one at a time: SHAPE (ShapeRectangles/ShapeMask), XFIXES (cursor visibility), RANDR, Xinerama, GE. Test each with xeyes/xterm before advertising the next.

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
XFILESEARCHPATH=/opt/X11/share/X11/%T/%N xcalc -rpn   # calculator (needs XFILESEARCHPATH)
xclock -analog             # Xaw widgets, arcs, timer events

# Verify version banner in console: "SwiftX11 v1.5.2"

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
- `X11LowLevel/cpp/X11Protocol/src/Ops/RenderOps.cpp` — RENDER extension implementation
- `X11LowLevel/cpp/X11Protocol/src/Ops/ExtensionOps.cpp` — Extension stubs (XFIXES, SHAPE, RANDR, etc.)
- `X11LowLevel/cpp/X11Protocol/src/Ops/SelectionOps.cpp` — Selection protocol + clipboard bridge
- `X11LowLevel/cpp/X11Protocol/src/Ops/PropOps.cpp` — Property operations + ClipboardCapture
- `X11LowLevel/include/SwiftX11Version.h` — Single source of truth for version string

---

## Important Conventions

- **All new code in C++ or swift** — no C files remain, no new C code
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
- **Trace tiers**: `#ifndef NDEBUG` for lifecycle traces always in debug; `X11_TRACE_<CATEGORY>` for categorical traces (RESIZE, PRESENT, LIFECYCLE, INPUT, RESOLVE); `X11_TRACE_VERBOSE` enables ALL categories. See `TraceDefs.hpp`.
