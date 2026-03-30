# SwiftX11 — Phase 9/10 Handoff (v1.19.35.24-dbg)

## Context

SwiftX11 is a native macOS X11 protocol server (Swift + C++). It implements 101 core X11 opcodes and 10 extensions. It successfully runs xterm, xeyes, xcalc, xclock, xfd, Xilinx Vivado 2025.1.1 (Java Swing), and Xilinx Vitis 2025.1.1 (Electron/Theia) from an AlmaLinux 9 Docker container.

**Vitis status**: Main window renders with native macOS title bar. `_NET_FRAME_EXTENTS` fixed to (0,0,0,0) — should fix Electron popup positioning (needs testing). Portal-GTK dialogs ("Set Workspace") render with inverted colors and are non-interactive (missing Composite extension). Cross-client event routing is now functional.

## Development Conventions

### Building & Testing
- **Build**: User builds from Xcode (Cmd+B on `SwiftX11` target). Do NOT use `xcodebuild` from CLI — it interferes with Xcode.app.
- **Git workflow**: Claude Code works in a worktree (`.claude/worktrees/<name>/`). Edit files there, commit, then merge to `develop`: `cd /Users/lkg/Documents/Vivado/SwiftX11/macos && git merge <branch> --no-edit`
- **CRITICAL**: Do NOT edit files directly in the main repo — Xcode has the project open and will crash if source files change under it. Always edit in the worktree.
- **Always merge to develop** before asking the user to test. Leave `main` alone until release.
- **Version bumps**: Edit `X11LowLevel/include/SwiftX11Version.h`. For debug iterations, increment `SWIFTX11_DEBUG_BUILD`. For releases, bump `SWIFTX11_VERSION_BASE` and set debug to 0.
- **Docker image**: `x64-linux-dbus` image bakes in `docker-entrypoint.sh`. After entrypoint changes: `cd /Users/lkg/Documents/Vivado/vivado2023 && docker build --platform linux/amd64 -t x64-linux-dbus -f Dockerfile .`

### Regression Tests
- `DISPLAY=127.0.0.1:1 xterm -sb -rightbar -bc` — text rendering, scrollbar, borders
- `DISPLAY=127.0.0.1:1 xeyes` — shape extension, motion tracking
- `DISPLAY=127.0.0.1:1 xcalc` — button widgets, focus, cursor
- Vivado: `./launch_vivado_sx11.sh` — Java Swing, dialogs, JidePopup, IP generation
- Vitis: `./launch_vitis_sx11.sh` — Electron/Theia IDE

## Priority Issues for Next Session

### 1. Vitis Menu Popup Coordinate Offset — FIX APPLIED, NEEDS TESTING (v1.19.35.24)

**Fix**: Set `_NET_FRAME_EXTENTS` to (0,0,0,0) for all windows. Root cause analysis: SwiftX11 is a non-reparenting WM — the window's X11 position already represents the content origin, not the frame origin. Previously we reported top=28, but Chromium/Electron subtracted frame extents from the window position to compute the frame origin for popup placement, resulting in popups 28px too high. With frame extents = (0,0,0,0), the client uses the X11 position as-is, which is correct.

**Changed files**: `WindowOps.cpp` (pushMapExtras), `XProtoServer.cpp` (flushPendingMaps)

**Test**: Launch Vitis, hover over File/Edit/View menus — dropdown popups should appear at the correct position aligned with the menu items.

**If fix doesn't work**: The issue might be that Electron positions popups using TranslateCoordinates or root coordinates from motion events rather than frame extents. In that case, investigate the exact Chromium popup placement code in `ui/views/widget/desktop_aura/desktop_window_tree_host_linux.cc`.

### 2. Portal-GTK Dialog Rendering & Interaction (HIGH)

The xdg-desktop-portal-gtk process (fd=19) creates the "Set Workspace" / "Open Folder" dialogs. These render with inverted colors (black where white should be) and are non-interactive (clicks don't register on buttons/lists).

**Root causes to investigate**:
- **Composite extension** (`present=0`) — GTK3 relies on Composite for widget compositing. Without it, GTK3 may use a broken fallback rendering path.
- **DAMAGE extension** (`present=0`) — often paired with Composite.
- **ARGB32 visual handling** — GTK3 creates ARGB32 windows for transparency. Verify our CreateWindow handles depth-32 correctly and that alpha isn't inverted.
- **Input routing** — cross-client button events now route correctly (v1.19.35.23), but verify the portal-gtk dialog's child windows receive ButtonPress events with correct coordinates.

**Tooltips**: OR popup windows from portal-gtk appear behind the dialog. Check stacking order — OR windows should be at `.floating` level.

### 3. XI2 XIQueryDevice Chromium Disconnect (HIGH — blocks Vitis with XI2)

Chromium/Electron disconnects after receiving XIQueryDevice reply. Wire format verified byte-perfect. Current workaround: hide XInputExtension (`present=0`). Vitis works without XI2.

**Next step**: Read Chromium's `ui/events/devices/x11/device_data_manager_x11.cc` to understand what post-XIQueryDevice validation causes the disconnect.

### 4. Ctrl+Click Regression (MEDIUM)

Ctrl+click no longer triggers button 3 in Vivado. Two-finger trackpad works. Regression in v1.17→v1.19.

## What Was Fixed This Session (v1.19.35.13 → v1.19.35.24)

| # | Fix | Description |
|---|-----|-------------|
| 1 | Native macOS title bar | MOTIF `decor=0` windows get `.titled` style — drag via title bar, traffic lights work |
| 2 | Click passthrough | Removed `isMovableByWindowBackground` which stole all mouseDown events |
| 3 | macOS→X11 clipboard | Track `sSelPushedCC` to prevent proactive capture from overwriting newer macOS content |
| 4 | Dialog focus | Clear WM_TAKE_FOCUS bounce tracking on DestroyWindow (XID reuse caused false suppression) |
| 5 | SwiftUI crash | Coalesce `@Published logText` updates via `DispatchQueue.main.async` batch |
| 6 | Log noise | Gate `[FLUSH]`, `[DISPATCH]`, `[PROP_TOPLEVEL]`, `[CREATE_TOPLEVEL]`, `[LABEL]`, `[HIERARCHY]` behind trace flags |
| 7 | PRIMARY selection | Don't steal ownership or send SelectionClear for PRIMARY — preserves text highlight in Vivado |
| 8 | Sequence wrap | Detect 16-bit wrap-around (>32768 requests during idle) and reset `max_wire_seq_` floor |
| 9 | Cross-client events | `sendEvent32`/`sendEventVariable` route to owning client's transport on fd mismatch |
| 10 | Cross-client crash | Send directly on target's `transport().sendAll()` — don't use `activateClient/deactivateClient` |
| 11 | libxkbfile | Added to Docker image for Vitis keyboard layout support |
| 12 | Frame extents fix | `_NET_FRAME_EXTENTS` set to (0,0,0,0) — fixes Chromium/Electron popup 28px offset (needs testing) |

## Resolved Issues from Previous Sessions

### XTEST GrabControl Crash (RESOLVED)
Root cause: `LD_PRELOAD` of `libgdk-x11-2.0.so.0` (GTK2) conflicted with GTK3 loaded by AT-SPI.

### JidePopup Decorated Windows (RESOLVED)
`_MOTIF_WM_HINTS` parsing. `decor=0` → borderless + floating for popups, titled for main windows.

### Dialog Window Management (RESOLVED)
`WM_TRANSIENT_FOR` parsed, transient dialogs shown via `makeKeyAndOrderFront`, `addChildWindow` abandoned.

### Stale Event Delivery on fd Reuse (RESOLVED)
Input events skipped when owning client isn't found.

### Wide Lines + Dashed Lines (RESOLVED)
`line_width > 1` and `line_style` (OnOffDash, DoubleDash). `CapNotLast` implemented.

### Cursor Font (RESOLVED)
`OpenFont("cursor")` loads `cursor.pcf.gz`.

### Auto-Ungrab on Window Destroy (RESOLVED)
`DestroyWindow`/`DestroySubwindows` call `removeForWindows()`.

### SO_SNDBUF Increase (RESOLVED)
TCP send buffer 4MB for Docker bridge.

## Docker Container Setup

- **Image**: `x64-linux-dbus` (AlmaLinux 9 + dbus + libxkbfile)
- **DISPLAY**: `host.docker.internal:1` (SwiftX11 on TCP port 6001)
- **dbus**: `dbus-launch --sh-syntax` in startup script, DISPLAY must be set BEFORE dbus-launch
- **No GTK2 preload**: Removed `libgdk-x11-2.0.so.0` from LD_PRELOAD (caused GTK2/3 conflict)
- **Vitis `--disable-gpu`**: Injected via entrypoint patching of Vitis wrapper (sed on line 567)
- **Xilinx volume**: `/Xilinx` Docker volume, read-only for `user`. Entrypoint patches are done as root.
- **Launch scripts**: `/Users/lkg/Documents/Vivado/vivado2023/` — `launch_vivado_sx11.sh`, `launch_vitis_sx11.sh`, `start_vivado_sx11.sh`, `start_vitis_sx11.sh`

## Version

Current: **v1.19.35.24-dbg** (debug build counter at 23). Set `SWIFTX11_DEBUG_BUILD` to 0 for release.

## Build

Open `macos/SwiftX11.xcodeproj` in Xcode. Build and run `SwiftX11` target. Test: `DISPLAY=127.0.0.1:1 xterm -sb -rightbar -bc`.

## Full Documentation

- Architecture: `docs/CLAUDE.md`
- Roadmap: `docs/TODO.md`
- README: `docs/README.md`
- License: `docs/LICENSE` (GPL-3.0)
