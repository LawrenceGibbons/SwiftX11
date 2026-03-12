# SwiftX11 — Session Handoff Prompt

**Date**: 2026-03-11
**Version**: v1.10.7
**Branch**: `develop++`

---

## How to Use This Document

Paste this into a new Claude conversation along with: "Please read CLAUDE.md and docs/TODO.md, then continue with the next task outlined in this handoff."

---

## Project Summary

SwiftX11 is an X11 protocol server running natively on macOS. It implements the X11 wire protocol so X11 clients (xterm, xeyes, xcalc, xclock, etc.) display via native Cocoa/Metal windows.

**Two language layers** (C layer fully eliminated in v1.0.0):
- **Swift** (`SwiftX11/`): UI owner — AppKit, Metal rendering, surface allocation, networking
- **C++** (`X11LowLevel/cpp/X11Protocol/`): Protocol core — request parsing, reply/event framing, raster ops, resource tables. `SwiftBridge.cpp` provides the `extern "C"` functions that Swift calls.

**Working clients**: xterm (scrollbar, cursor blink, modifier keys, Option+click thumb drag, bidirectional clipboard, CoreText antialiased fonts, Xft subpixel rendering), xeyes (SHAPE transparency), xcalc (Symbol font sqrt/pi/division), xclock, multi-client, multi-monitor with popup menus

**Advertised extensions (7)**: BIG-REQUESTS (133), RENDER (139), XFIXES (134), SHAPE (135), RANDR (136), XINERAMA (137), GE (138)

**Display**: SwiftX11 listens on display :1 (TCP port 6001 + Unix socket `/tmp/.X11-unix/X1`). `~/.profile` sets `DISPLAY=127.0.0.1:1`.

---

## What Was Accomplished Recently (v1.10.0 - v1.10.7)

### v1.10.7: Multi-Monitor Popup Menu Fix
- **Blank popup text on external monitors**: `CAMetalLayer.contentsScale` stayed 0.0 for `.borderless` NSWindows on external monitors. Fixed by explicit `backingScaleFactor` propagation in `ensureMetalSetup()` + `viewDidChangeBackingProperties`.
- **Hot-plug popup positioning**: xterm doesn't query RANDR, so Xlib's `WidthOfScreen`/`HeightOfScreen` remain stale after monitor changes. Fixed with server-side `adjustOROriginForCursorScreen()` + `x11_set_window_position()` to sync X11 geometry.
- **ScreenLayoutChanged host command**: `CGDisplayReconfigurationCallback` now broadcasts ConfigureNotify + RRScreenChangeNotify to all connected clients on monitor hot-plug/unplug.
- **Diagnostic trace cleanup**: Removed ~217 lines of verbose per-frame OR popup traces.

### v1.10.0: Rendering Performance + Metal-Only
- **Metal rendering required**: Software (CGImage/CALayer) rendering path removed entirely.
- **PutImage bulk memcpy**: GXcopy fast path uses `std::memcpy` + 4-pixel-unrolled alpha forcing.
- **Expose two-pass**: Backgrounds/borders filled first, then Expose events sent.

### v1.9.7: Multi-Monitor Support
- ScreenLayout cache, dynamic RANDR/Xinerama, coordinate conversion fixes
- `xrandr --query` works with real monitor configuration

---

## Known Issues (deferred)

- **xcalc -rpn extra button labels**: Buttons 40-54 show widget names. NOT a server bug -- app-defaults only cover 1-39.
- **xclock/xcalc FontSet warnings**: "Missing charsets in String to FontSet conversion" -- XCreateFontSet() expects multiple charset fonts.
- **xeyes shaped window occasional black flash on resize**: Minor cosmetic issue during live resize.

---

## Next Task: Phase 5.3 — Keyboard

**Priority**: MEDIUM — needed for Vivado/Vitis (Java Swing expects comprehensive keysym tables and modifier mapping).

### Unchecked items from docs/TODO.md Phase 5.3:
- [ ] **Full keysym mapping**: Map macOS virtual keycodes to X11 keysyms comprehensively (currently minimal)
- [ ] **Modifier mapping**: GetModifierMapping should return a mapping that matches macOS keyboard layout
- [ ] **Keymap state**: QueryKeymap returns current key state (currently all-zeros stub)
- [ ] **XKB (optional)**: Modern clients may query for XKB extension — return not-present is acceptable initially

### Suggested approach:
1. Audit current `sendKeyEvent()` implementation — what keysyms are mapped, what's missing
2. Build comprehensive macOS virtual keycode → X11 keysym table (Latin-1 + function keys + keypad + special keys)
3. Implement GetModifierMapping with correct macOS→X11 modifier semantics (Cmd→Mod4, Option→Mod1, etc.)
4. Implement GetKeyboardMapping to return the keysym table
5. Test with `xev -event keyboard` to verify all keys produce correct keysyms
6. Test with `xterm` for text input, special keys (Home/End/Page/arrows/F-keys)
7. Consider XKB stubs if Java or GTK clients query for it

### Other unchecked TODO items (lower priority):
- Phase 4.4: Colormap verification (TrueColor visual, AllocColor)
- Phase 5.2: Xauth, latency tolerance
- Phase 5.5: Window menu integration
- Phase 5.6: ICCCM/WM compliance (WM_HINTS, WM_NORMAL_HINTS, EWMH)
- Phase 7: XInput2, SYNC, Shape AA

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
xev -event keyboard            # Keyboard event testing (for Phase 5.3)

# Verify version banner: "SwiftX11 v1.10.7"

# Test multi-monitor popups:
# 1. Open xterm on external monitor
# 2. Right-click (Ctrl+click) for popup menu -> should show text, highlight works
# 3. Hot-plug/unplug monitor -> popup should follow cursor screen
```

---

## Key Architecture References

Read these files to understand the codebase:
- `CLAUDE.md` -- Comprehensive architecture doc (surface routing, damage pipeline, input events, development guidelines)
- `docs/TODO.md` -- Full roadmap with testing apps and priority order
- `X11LowLevel/include/SwiftX11Version.h` -- Single source of truth for version string
- `X11LowLevel/cpp/X11Protocol/include/Utils/TraceDefs.hpp` -- Categorical trace system
- `X11LowLevel/cpp/X11Protocol/include/Core/ScreenLayout.hpp` -- Multi-monitor layout cache
- `SwiftX11/Core/WindowRegistry.swift` -- Window management, popup adjustment, coordinate conversion

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
- **Trace tiers**: `#ifndef NDEBUG` for lifecycle traces; `X11_TRACE_<CATEGORY>` for categorical; `X11_TRACE_VERBOSE` enables ALL. See `TraceDefs.hpp`.
