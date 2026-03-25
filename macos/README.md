# SwiftX11

A native macOS X11 protocol server. X11 clients render into native Cocoa/Metal windows with no XQuartz dependency.

SwiftX11 implements the X11 wire protocol directly, enabling X11 applications — including Xilinx Vivado running in a Linux Docker container — to display on macOS with native window management, Metal-accelerated rendering, and macOS clipboard integration.

**Current version:** v1.19.33

## Features

- **80+ X11 core opcodes** — window management, drawing, events, properties, selections, fonts
- **10 extensions** — SHAPE, RANDR, Xinerama, RENDER, XI2, XTEST, XFIXES, BIG-REQUESTS, XC-MISC, Generic Event
- **Metal rendering** with partial texture uploads and 20ms damage coalescing
- **macOS clipboard bridge** — copy/paste between X11 and macOS apps via NSPasteboard
- **Multi-monitor** — real CGDisplay data, dynamic RANDR/Xinerama with reconfiguration callbacks
- **Multi-client** — concurrent X11 connections with per-client resource tracking
- **Rootless windows** — each top-level X11 window is a native NSWindow
- **Font system** — 21 bundled BDF fonts, system PCF loading, macOS CoreText bridge, XLFD glob matching
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
| Xilinx Vivado 2025.1 | Working | Java Swing, full IP workflow, dialogs, popups |

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

### Protocol

- **Little-endian only** — big-endian client connections are rejected at handshake. All practical X11 clients on modern hardware are little-endian.
- **INCR protocol** — not implemented. Large clipboard transfers (>65KB) are silently truncated. Affects large copy/paste in Vivado.
- **MULTIPLE selection target** — not implemented. Multi-target clipboard requests (e.g., `xsel -m`) fail.
- **AllowEvents / Sync grabs** — all grabs behave as async. The sync/freeze event queue is not implemented. No known client depends on this.
- **GrabKey** — keyboard passive grabs are stubbed. Accessibility tools that use keyboard grabs won't work.
- **XKB (X Keyboard Extension)** — not advertised. Clients fall back to core keyboard protocol, which works correctly.
- **Xauth** — not implemented. Authentication is not required for local display `:1`.

### Rendering

- **Join styles** — line join_style (Miter/Round/Bevel) is stored but not applied. All line joins are square.
- **CapRound / CapProjecting** — not implemented for wide lines. CapNotLast and CapButt work.
- **GC subwindow_mode** — IncludeInferiors is not implemented. All drawing clips to child windows (ClipByChildren).
- **FillPoly winding rule** — only EvenOddRule. WindingRule is ignored.
- **GetImage XYPixmap** — only ZPixmap format supported.
- **DAMAGE extension** — not advertised. Internal damage tracking works; DamageNotify events are not sent.

### Window Management

- **Ctrl+click** — does not reliably trigger right-click (button 3) in some contexts. Two-finger trackpad click works as a workaround.
- **Pointer coordinate offset after left-edge resize** — menu selections may be offset horizontally after resizing a window by dragging its left edge.

## Extensions

| Extension | Version | Status |
|-----------|---------|--------|
| SHAPE | 1.1 | Full — pixel-level clipping, bounding/clip/input shapes |
| RANDR | 1.3 | Full — dynamic multi-monitor with real display data |
| Xinerama | 1.1 | Full — per-monitor screen entries |
| RENDER | 0.11 | Partial — PictFormats, Composite, FillRectangles, SolidFill, Glyphs |
| XI2 (XInput2) | 2.0 | Partial — RawMotion, DeviceInfo, XI events |
| XTEST | 2.2 | Full — GetVersion, FakeInput, CompareCursor, GrabControl |
| XFIXES | 5.0 | Partial — QueryVersion, SelectionNotify |
| BIG-REQUESTS | — | Full — max 4MB requests |
| XC-MISC | — | Full — XID range recycling |
| Generic Event | 1.0 | Minimal — event wrapper infrastructure |

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
