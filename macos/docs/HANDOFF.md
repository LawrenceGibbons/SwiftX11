# SwiftX11 Session Handoff

Last updated: 2026-03-16 (v1.13.2)

## Current State

SwiftX11 is a working X11 server for macOS. Vivado (Java Swing) is confirmed working. Vitis (Eclipse SWT/GTK) connects and starts but needs more testing. The app has a custom icon, Stage Manager support, graceful quit, and multi-monitor window placement.

**Branch**: `develop++` — all work is on this branch
**Version**: 1.13.2 (defined in `X11LowLevel/include/SwiftX11Version.h`)
**Bundle ID**: `com.rlan.SwiftX11` (changed from `RLAN.SwiftX11` in v1.13.2 to fix poisoned macOS icon cache)

## Build & Run

```bash
cd /Users/lkg/Documents/Vivado/SwiftX11/macos
# Build from Xcode: open SwiftX11.xcodeproj, Cmd+R
# Or command line:
xcodebuild -project SwiftX11.xcodeproj -scheme SwiftX11 -configuration Debug build
```

Test clients: `DISPLAY=127.0.0.1:1 xterm -sb -rightbar -bc`

## Next Tasks (Priority Order)

### 1. Help Menu / User Guide (Phase 6.1 in TODO.md)
Add a Help menu item that opens documentation covering:
- **DISPLAY setup**: `DISPLAY=127.0.0.1:1` for TCP, `DISPLAY=:1` for Unix socket, `~/.profile` configuration
- **Font locations**: `/opt/X11/share/fonts/{misc,75dpi,100dpi}/`, CoreText bridge (fixed->Menlo, courier->Courier, etc.), antialiased font toggle in Settings -> Rendering
- **Settings panels**: Rendering options, Network tab (TCP/Unix toggles), Docker usage
- **Log window**: View menu Show/Hide toggle, what the log output means, trace categories
- **Docker/container workflow**: `DISPLAY=host.docker.internal:1`, TCP vs Unix socket on Docker Desktop
- **Keyboard shortcuts**: Option+click = middle mouse (scrollbar thumb drag), Ctrl+click = right-click, Cmd+W = close/kill client
- **Known limitations**: No GLX, no XKB compose, big-endian rejected

The help could be an NSWindow with an NSTextView (rich text), or a simple HTML file loaded in a WKWebView. SwiftUI sheet anchored to the main menu is another option.

### 2. XC-MISC Extension (Phase 7.1 — HIGH)
Prevents XID exhaustion crash for long-running Vivado sessions. Very simple:
- `XC-MiscGetVersion` → return 1.1
- `XC-MiscGetXIDRange` → return new XID range from server's free pool
- `XC-MiscGetXIDList` → return individual free XIDs
Needs: XID allocation tracking per client, new extension handler in ExtensionOps.cpp

### 3. XInput2 Stubs (Phase 7.2 — MEDIUM-HIGH)
GTK3/4 queries XI2 at startup. Without it, GTK falls back but loses features:
- `XIQueryVersion` → return 2.0+ or present=0
- `XIQueryDevice` → list virtual core pointer + keyboard
- `XISelectEvents` → accept and track selections
- Test: run `gtk3-demo` and see if it works without XI2

### 4. XTEST Extension (Phase 7.3 — MEDIUM)
Automation/accessibility. Used by xdotool, AT-SPI:
- `XTestFakeInput` → synthesize events through InputState
- `XTestGrabControl` → allow events to bypass grabs

### 5. Vitis Testing
Eclipse SWT/GTK from ALMA 9 container. Run script at `~/Documents/Vivado/vivado2023/run_vitis_swiftx11.sh`. May uncover additional extension/protocol gaps.

## Key Files to Know

| Purpose | File |
|---------|------|
| App entry + SwiftUI scene | `SwiftX11/App.swift` |
| AppDelegate (menus, quit, icon) | `SwiftX11/AppDelegate.swift` |
| Window creation | `SwiftX11/UI/Windows/X11WindowController.swift` |
| Metal rendering + surface | `SwiftX11/UI/Windows/X11WindowHost.swift` |
| Metal draw pipeline | `SwiftX11/UI/Windows/X11MetalRenderer.swift` |
| Window registry (Cocoa side) | `SwiftX11/Core/WindowRegistry.swift` |
| Settings store | `SwiftX11/Core/SettingsStore.swift` |
| Status bar controller | `SwiftX11/UI/StatusBar/StatusItemController.swift` |
| Version string | `X11LowLevel/include/SwiftX11Version.h` |
| C++ extension handlers | `X11LowLevel/cpp/X11Protocol/src/Ops/ExtensionOps.cpp` |
| Full architecture docs | `CLAUDE.md` |
| Roadmap | `docs/TODO.md` |

## Recent Changes (v1.13.x)

- **XFIXES/RANDR stubs**: Minor opcodes from Vitis that were returning BadRequest now handled
- **Multi-monitor window placement**: Default-position windows go to NSScreen.main, not virtual desktop top-left
- **View menu dynamic toggle**: Show/Hide Log Window updates based on window visibility (AppDelegate NSMenu + NSMenuDelegate)
- **Graceful quit**: `applicationShouldTerminate` stops X11 server before window teardown (no more beach ball)
- **App icon**: Generated from SwiftX11-2.pdf, 10 sizes with blue background
- **Bundle ID**: Changed to `com.rlan.SwiftX11` — old ID had poisoned icon cache in macOS
- **NSWindow.sharingType = .readWrite**: Set on all X11 windows

## Architecture Quick Reference

- **Swift** owns: AppKit windows, Metal rendering, surface allocation, networking
- **C++** owns: X11 protocol parsing, drawing ops, resource tables, event delivery
- **Bridge**: `extern "C"` functions in `SwiftBridge.cpp` / `SwiftX11Bridge.h`
- **Surfaces**: Swift allocates host surface → registers via `x11_surface_update()` → C++ draws into it → damage reported → Metal presents
- **Extensions**: 7 advertised (BIG-REQUESTS, RENDER, XFIXES, RANDR, XINERAMA, GE, SHAPE). RENDER not fully advertised (missing Trapezoids rendering). New extensions go in `ExtensionOps.cpp` with opcode in `X11ExtOpcodes.hpp`.

## Vitis Run Script

Located at `~/Documents/Vivado/vivado2023/run_vitis_swiftx11.sh`. Features:
- Starts SwiftX11 if not running
- Runs Vitis in Docker container with DISPLAY forwarded
- `monitor_swiftx11()` background function: if SwiftX11 quits, kills container after timeout
- Cleanup function kills monitor and stops container gracefully
