# SwiftX11

A native macOS X11 protocol server. X11 clients render into native Cocoa/Metal windows with no XQuartz dependency.

SwiftX11 implements the X11 wire protocol directly, enabling X11 applications — including Xilinx Vivado running in a Linux Docker container — to display on macOS with native window management, Metal-accelerated rendering, and macOS clipboard integration.

**Current version:** v2.0.0

## Recent Changes

**v2.0.0** makes the root a real window and completes the 2026-09-08 xorg-comparison review (R0–R5):

- **The root window is now a real window (XID `0x2`).** This is a wire-visible change — the advertised root ID moved from `1` to `0x2` — which is why v2.0.0 is a major version bump. Root now reports geometry, delivers `SubstructureNotify` to observers (`xev -root`, `wmctrl`), is the last window on the input-delivery walk, accepts per-client XI2 selection, and interprets EWMH/ICCCM client messages (`_NET_ACTIVE_WINDOW`, `_NET_WM_STATE`, `WM_CHANGE_STATE`) as the rootless window manager.
- **XKEYBOARD dynamic remap.** The served keymap now rebuilds when a client changes the core mapping (e.g. `xmodmap`), broadcasting `XkbMapNotify` — XKB-path clients no longer keep the boot keymap.
- **Window management.** `ChangeSaveSet` (client embedding), cross-client window parents, and the `InputOnly` window class are now honoured; `ReparentWindow` emits the full Unmap → Reparent → Map choreography.
- **Wire hygiene.** BIG-REQUESTS raised to ~16 MB, honest extension advertisement (XFIXES negotiated to 1.0), unknown opcodes answer `BadRequest`.

Highlights carried over from v1.21.0 (since v1.20.0):

- **XInput2 (XI2) — full Stage 2, on by default.** Per-client event selection and fan-out, raw events, active/passive grabs, and crossing/focus semantics verified against the xorg-server source (Phases A–G). Fixes Vitis menus and portal-GTK dialogs.
- **XKEYBOARD (XKB) extension — on by default.** Byte-exact GetMap/GetNames/GetControls/GetCompatMap, per-client event selection, detectable autorepeat, and `_XKB_RULES_NAMES`. Required for GTK3 to receive keys over XI2; verified live against XQuartz's libX11 1.8.
- **Comprehensive keyboard mapping** — full macOS virtual-keycode → X11 keysym coverage (letters, digits, punctuation, F1–F20, keypad, navigation, left/right modifiers), 4-column GetKeyboardMapping for Swing/GTK.
- **Clipboard** — INCR protocol for large (>64KB) transfers, bidirectional X11 ↔ macOS sync (12MB buffers).
- **Rendering** — ARGB32 component-alpha (subpixel) glyphs, RENDER picture clip rectangles, direction-aware CopyArea overlap.
- **Input correctness (2026-09-08 review, R0 + R2)** — passive button- and keyboard-grab activation (`GrabButton`/`GrabKey`), `WarpPointer` now generates motion/crossing events, `GrabServer` suspends other clients, `MappingNotify` for all three mapping requests, `XkbStateNotify` on Caps Lock, plus a batch of wire-hygiene fixes (PolyText font shifts, CHAR2B text extents, zero-length properties, selection-owner cleanup).
- **Ctrl+click → right-click** and Option+click → middle-click made reliable.

See `docs/CLAUDE.md` for the detailed version log.

## Features

- **100+ X11 core opcodes** — window management, drawing, events, properties, selections, fonts, keyboard/pointer mapping
- **12 extensions advertised** — SHAPE, RANDR, Xinerama, RENDER, XI2 (XInput2), XKEYBOARD, XTEST, XFIXES, Composite, BIG-REQUESTS, XC-MISC, Generic Event (DAMAGE has handlers but is deliberately not advertised — a rootless server generates no DamageNotify)
- **Metal rendering** with partial texture uploads and 20ms damage coalescing
- **macOS clipboard bridge** — bidirectional copy/paste via NSPasteboard, INCR protocol for large transfers
- **Full keyboard support** — macOS virtual-keycode → X11 keysym mapping, XKEYBOARD, passive key grabs
- **Multi-monitor** — real CGDisplay data, dynamic RANDR/Xinerama with reconfiguration callbacks
- **Multi-client** — concurrent X11 connections with per-client resource tracking and event selection
- **Rootless windows** — each top-level X11 window is a native NSWindow
- **Font system** — 21 bundled BDF fonts, system PCF loading, macOS CoreText bridge (subpixel glyphs), XLFD glob matching
- **Wide/dashed lines** — line_width, OnOffDash/DoubleDash, CapNotLast/CapButt
- **Window type support** — `_NET_WM_WINDOW_TYPE`, `_MOTIF_WM_HINTS`, `WM_TRANSIENT_FOR`
- **Stage Manager compatible** — transient dialogs appear in the correct Stage Manager stage

### Tested Clients

| Client | Status | Notes |
|--------|--------|-------|
| xterm | Working | Scrollbar, keyboard, mouse, Option+click for middle button |
| xeyes | Working | SHAPE extension, motion tracking |
| xcalc | Working | Symbol fonts, button widgets |
| xclock | Working | Timer updates |
| xfd | Working | Font display with cursor and text fonts |
| Xilinx Vivado 2025.1 | Working | Java Swing, full IP workflow, dialogs, popups, hw_ila drag |
| Xilinx Vitis 2025.1 | Working | Electron/Chromium + GTK, menus, portal-GTK file dialogs (over XI2) |
| License Manager | Working | Cairo/AWT subpixel glyphs, scrollbars |

## Build

Requires macOS and Xcode 14+ (Swift 5.7, C++17).

```bash
open macos/SwiftX11.xcodeproj
# Build target: SwiftX11
# Cmd+B to build, Cmd+R to run
```

The app starts on display `:1` (to avoid conflict with XQuartz on `:0`).

## Quick Start

```bash
# Set display (add to ~/.profile for persistence)
export DISPLAY=127.0.0.1:1

# Run SwiftX11 from Xcode, then:
xterm -sb -rightbar -bc    # terminal with scrollbar
xeyes                       # pointer tracking test
xcalc                       # calculator with symbol fonts
```

### Docker (Vivado/Vitis)

SwiftX11 is designed to serve X11 from Linux containers running on macOS:

```bash
docker run --rm \
  -e DISPLAY=host.docker.internal:1 \
  -e TZ="America/New_York" \
  --platform linux/amd64 \
  your-vivado-image bash /home/user/start_vivado.sh
```

The container needs standard X11 client libraries (libX11, libXext, libXrender, etc.) and dbus for GTK/AT-SPI support. See `docs/CLAUDE.md` for detailed container setup.

## Architecture

```
Swift (AppKit/Metal)          C++ (X11 Protocol)
  NSWindow, NSView              Request parsing
  Metal texture uploads         Reply/event framing
  Surface allocation            Resource tables
  Input event capture           Drawing operations
         |                            |
         +--- SwiftBridge.cpp (extern "C") ---+
```

- **Swift** owns all UI: window creation, surface buffers, Metal/software rendering, NSEvent handling
- **C++** owns the protocol: parsing, resource management, drawing into Swift-allocated surfaces
- **Surfaces** are keyed by top-level window XID; child windows draw at offsets into the host surface
- **Damage** accumulates in a shared mutex-protected rect, consumed at present time for partial Metal uploads

## Known Limitations

The public-facing summary with workarounds is in [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md); the detail below is grouped by area.

### Protocol

- **Little-endian only** — big-endian client connections are rejected at handshake. All practical X11 clients on modern hardware are little-endian.
- **MULTIPLE selection target** — not implemented. Multi-target clipboard requests (e.g., `xsel -m`) fail.
- **AllowEvents / Sync grabs** — all grabs behave as async. The sync/freeze event queue is not implemented. No known client depends on this.
- **Xauth** — not implemented. Authentication is not required for local display `:1`.

### Rendering

- **Join styles** — line join_style (Miter/Round/Bevel) is stored but not applied. All line joins are square.
- **CapRound / CapProjecting** — not implemented for wide lines. CapNotLast and CapButt work.
- **GC subwindow_mode** — IncludeInferiors is not implemented. All drawing clips to child windows (ClipByChildren).
- **FillPoly winding rule** — only EvenOddRule. WindingRule is ignored.
- **GetImage XYPixmap** — only ZPixmap format supported.
- **DAMAGE extension** — not advertised. Internal damage tracking works; DamageNotify events are not sent.

### Window Management

- **Cooperative activation (macOS 14+)** — a newly mapped window comes to the front and takes focus on a real user click, but not when SwiftX11 is only frontmost via scripting or an Xcode launch. This is macOS's cooperative-activation policy, not an X11 issue; normal use is unaffected.

## Extensions

| Extension | Version | Status |
|-----------|---------|--------|
| SHAPE | 1.1 | Full — pixel-level clipping, bounding/clip/input shapes |
| RANDR | 1.3 | Full — dynamic multi-monitor with real display data |
| Xinerama | 1.1 | Full — per-monitor screen entries |
| RENDER | 0.11 | Partial — PictFormats, Composite, Trapezoids/Triangles, gradients, component-alpha glyphs, picture clips |
| XI2 (XInput2) | 2.2 | Full — per-client selection/fan-out, raw events, active/passive grabs, crossing/focus semantics (default on) |
| XKEYBOARD | 1.0 | Full — GetMap/GetNames/GetControls/GetCompatMap, SelectEvents, StateNotify, MapNotify (dynamic remap), detectable autorepeat (default on) |
| XTEST | 2.2 | Full — GetVersion, FakeInput, CompareCursor, GrabControl |
| XFIXES | 1.0 | Partial — QueryVersion (negotiated to 1.0), SelectionNotify, ChangeSaveSet |
| Composite | 0.4 | Minimal — advertised, redirect stubs |
| DAMAGE | 1.1 | Handlers present but **not advertised** — internal damage tracking works, no DamageNotify (rootless server has no compositor to feed) |
| BIG-REQUESTS | — | Full — max ~16 MB requests (4,194,303 words) |
| XC-MISC | — | Full — XID range recycling |
| Generic Event | 1.0 | Full — GenericEvent (35) dispatch for XI2 cookies |

## Diagnostics

### Wire Trace

Enable in the SwiftX11 UI: toggle "Wire Trace (stderr)". Every incoming request and outgoing packet is logged to Xcode console.

### Ring Buffer

On client disconnect, the last 16 dispatched requests and 32 outgoing packets are dumped to the log window (verbosity level 1).

### Log Window

The built-in log window supports search (Cmd+F or Find button) and adjustable verbosity (0-3).

### Build-time Traces

Add `-DX11_TRACE_VERBOSE` to `OTHER_CPLUSPLUSFLAGS` in Xcode build settings for high-frequency per-operation traces.

## Installation

### Installer Package (recommended)

Download the latest `SwiftX11-{VERSION}.pkg` from the [Releases](../../releases) page. The installer provides:

- **SwiftX11.app** — installs to `/Applications`
- **X11 Fonts** (optional) — standard PCF bitmap fonts installed to `/opt/X11/share/fonts/`. Not needed if XQuartz fonts are already present; the installer detects existing fonts and deselects this option automatically.

On first launch, you may need to right-click the app and select "Open" to bypass Gatekeeper (the installer is unsigned).

After installation, add to your shell profile (`~/.zprofile` or `~/.profile`):

```bash
export DISPLAY=127.0.0.1:1
```

### Building the Installer

To build the `.pkg` installer from source:

```bash
bash macos/scripts/build-installer.sh
```

This reads the version from `SwiftX11Version.h`, builds a Release configuration via Xcode, stages the app and font payloads, and produces `SwiftX11-{VERSION}.pkg` in `macos/build/`.

Prerequisites: Xcode command line tools. Font bundling requires X11 fonts at `/opt/X11/share/fonts/` (from XQuartz or a previous SwiftX11 install).

### Building from Source

See the [Build](#build) section above. For development, build the Debug configuration in Xcode and run directly.

## Documentation

- **Architecture & development guide:** `docs/CLAUDE.md`
- **Roadmap & audit status:** `docs/TODO.md`

## License

This project is licensed under the **GNU General Public License v3.0** (GPL-3.0). See [LICENSE](../LICENSE) for the full text.

Copyright (c) 2026 Lawrence Gibbons.
