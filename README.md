# SwiftX11 — X11 Server for macOS

SwiftX11 is a native macOS X11 protocol server built with Swift and C++. It implements the X11 wire protocol so that X11 clients can display and interact via native Cocoa windows with Metal-accelerated rendering.

## Why SwiftX11?

macOS no longer ships with an X11 server. [XQuartz](https://www.xquartz.org) fills this gap but uses XCB/Xlib internals originally designed for Unix. SwiftX11 takes a different approach — implementing the X11 protocol from scratch as a native macOS app, with Swift owning all UI surfaces and C++ handling protocol parsing and raster operations.

**Primary use case**: Running [Xilinx Vivado](https://www.xilinx.com/products/design-tools/vivado.html) and [Vitis](https://www.xilinx.com/products/design-tools/vitis.html) from a Linux container on macOS, where XQuartz's SubstructureRedirect timing causes dialog sizing issues.

## Features

- **Native Metal rendering** — GPU-accelerated compositing, partial texture uploads, shaped window transparency
- **Full X11 core protocol** — 100+ opcodes: window management, drawing operations, events, properties, selections, atoms, fonts, cursors, colormaps
- **10 X11 extensions** — BIG-REQUESTS, RENDER, XFIXES, RANDR, XINERAMA, GE, SHAPE, XC-MISC, XTEST, Composite (XInput2/XI2 handlers are implemented but not advertised, for Electron compatibility)
- **ICCCM/EWMH compliance** — WM_NORMAL_HINTS, WM_HINTS, WM_TAKE_FOCUS, WM_DELETE_WINDOW, _NET_WM_WINDOW_TYPE, _NET_WM_STATE, _NET_FRAME_EXTENTS
- **Font support** — PCF/BDF bitmap fonts, CoreText bridge for system fonts with antialiasing toggle
- **Multi-monitor** — Dynamic RANDR/Xinerama with real display data, per-monitor DPI, hot-plug support
- **Clipboard bridge** — Bidirectional X11 ↔ macOS clipboard sync (CLIPBOARD and PRIMARY selections)
- **Container networking** — TCP (for Docker via `host.docker.internal:1`) and Unix socket (`/tmp/.X11-unix/X1`)
- **SubstructureRedirect emulation** — Proper WM-style window sizing at map time, handling clients that create windows at 1×1 and configure later
- **Non-rectangular windows** — Full SHAPE extension with transparent backgrounds (xeyes works)

## Requirements

- macOS 14+ (Sonoma or later)
- Metal-capable GPU (all Macs since 2012)
- X11 client libraries installed (via [XQuartz](https://www.xquartz.org) or [Homebrew](https://brew.sh): `brew install libx11`)

## Installation

### From the .dmg

Download the latest `.dmg` from [Releases](https://github.com/LawrenceGibbons/SwiftX11/releases), open it, and drag **SwiftX11** to Applications. X11 bitmap fonts are bundled inside the app — no XQuartz required for the server itself.

> **Note**: The app is ad-hoc signed but not notarized — right-click → Open to bypass Gatekeeper on first launch.

### From Source

```bash
git clone https://github.com/LawrenceGibbons/SwiftX11.git
cd SwiftX11
open macos/SwiftX11.xcodeproj
# Build and run the SwiftX11 target
```

## Quick Start

1. Launch **SwiftX11** from Applications
2. Add to your shell profile (`~/.zprofile` or `~/.profile`):
   ```bash
   export DISPLAY=127.0.0.1:1
   ```
3. Run an X11 app:
   ```bash
   xterm &
   xeyes &
   xcalc &
   ```

### Docker / Linux Containers

SwiftX11 listens on TCP port 6001 (display :1), bound to `0.0.0.0`:

```bash
docker run -e DISPLAY=host.docker.internal:1 my-image
```

For native macOS X11 clients, Unix socket also works:
```bash
export DISPLAY=:1
```

## Building the DMG

```bash
bash macos/scripts/build-dmg.sh
```

This builds the Release configuration and packages `SwiftX11.app` (with bundled X11 fonts) into a distributable `.dmg`.

## Architecture

```
┌─────────────────────────────────────────────────┐
│  Swift (macos/SwiftX11/)                        │
│  AppKit windows, Metal rendering, input events, │
│  networking (owns UI, not pixel buffers)        │
├─────────────────────────────────────────────────┤
│  extern "C" bridge (SwiftBridge.cpp)            │
├─────────────────────────────────────────────────┤
│  C++ (macos/X11LowLevel/cpp/X11Protocol/)       │
│  Wire protocol, raster drawing, resource tables,│
│  WM emulation, host pixel-buffer ownership      │
└─────────────────────────────────────────────────┘
```

C++ owns all host window surfaces — CPU pixel buffers held in `DrawableSurfaceRegistry` (since v1.19.35.49). Swift requests sizes and uploads the buffers to Metal textures at present time; it never holds a reference to the live buffer. Child windows draw into their host's surface at an offset — no per-child allocation.

See [CLAUDE.md](macos/CLAUDE.md) for detailed architecture documentation.

## Tested Applications

| App | Status | Notes |
|-----|--------|-------|
| xterm | ✅ Working | Scrollbar, cursor blink, Option+click thumb drag |
| xeyes | ✅ Working | SHAPE extension, transparent background |
| xcalc | ✅ Working | Including RPN mode |
| xclock | ✅ Working | Analog and digital |
| Vivado | ✅ Working | Full GUI, menus, dialogs, banner, clipboard, hardware-manager drag |
| Vitis | ✅ Working | Electron/Eclipse + GTK portal file dialogs, over TCP from a container |

## Display Number

SwiftX11 runs on **display :1** (TCP port 6001) to avoid conflict with XQuartz on :0. Set `DISPLAY=127.0.0.1:1` in your shell profile.

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE). Any derivative works must also be distributed under the GPL v3.

## Acknowledgments

Built with extensive assistance from [Claude](https://claude.ai) (Anthropic).
