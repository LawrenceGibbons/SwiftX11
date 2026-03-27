# SwiftX11 — Phase 8/9 Handoff (v1.19.35)

## Context

SwiftX11 is a native macOS X11 protocol server (Swift + C++). It implements 101 core X11 opcodes and 10 extensions. It successfully runs xterm, xeyes, xcalc, xclock, xfd, Xilinx Vivado 2025.1.1 (Java Swing), and Xilinx Vitis 2025.1.1 (Electron/Theia) from an AlmaLinux 9 Docker container.

## Development Conventions

### Building & Testing
- **Build**: User builds from Xcode (Cmd+B on `SwiftX11` target). Do NOT use `xcodebuild` from CLI — it interferes with Xcode.app.
- **Git workflow**: Claude Code works in a worktree (`.claude/worktrees/<name>/`). Commit there, then merge to `develop` for the user to test: `cd /Users/lkg/Documents/Vivado/SwiftX11/macos && git merge <branch> --no-edit`
- **Always merge to develop** before asking the user to test. Tell them explicitly about the merge step.
- **Version bumps**: Edit `X11LowLevel/include/SwiftX11Version.h`. For debug iterations, increment `SWIFTX11_DEBUG_BUILD`. For releases, bump `SWIFTX11_VERSION_BASE` and set debug to 0.
- **Docker image**: `x64-linux-dbus` image bakes in `docker-entrypoint.sh`. After entrypoint changes: `cd /Users/lkg/Documents/Vivado/vivado2023 && docker build --platform linux/amd64 -t x64-linux-dbus -f Dockerfile .`

### Regression Tests
- `DISPLAY=127.0.0.1:1 xterm -sb -rightbar -bc` — text rendering, scrollbar, borders
- `DISPLAY=127.0.0.1:1 xeyes` — shape extension, motion tracking
- `DISPLAY=127.0.0.1:1 xcalc` — button widgets, focus, cursor
- Vivado: `./launch_vivado_sx11.sh` — Java Swing, dialogs, JidePopup, IP generation
- Vitis: `./launch_vitis_sx11.sh` — Electron/Theia IDE

## Primary Issue: XI2 XIQueryDevice Disconnect (HIGH)

### The Problem

Chromium/Electron (Vitis 2025.1.1) disconnects immediately after receiving our XIQueryDevice (XI2 minor 48) reply. The disconnect blocks Vitis from working with XInputExtension enabled.

### Current Workaround

XInputExtension is hidden from Chromium (`present=0` in QueryExtension reply). Vitis works perfectly without XI2 — Electron falls back to core X11 input. This workaround is in `ExtensionOps.cpp` in the XInput2 handler for QueryExtension.

**Important**: Vitis works without issue under our xpra implementation with full XI2 support. This confirms the problem is specific to SwiftX11's XI2 implementation, not the Docker/Rosetta/Electron environment.

### What Has Been Exhaustively Verified

| Test | Result |
|------|--------|
| Full hex dump of reply vs XI2proto.h structs | Byte-perfect match |
| Zero devices | Still disconnects |
| Masters only (2 devices) | Still disconnects |
| Pointer only (1 device) | Still disconnects |
| XI version 2.0 vs 2.2 | Both disconnect |
| ValuatorClass Relative vs Absolute mode | Both disconnect |
| Added ScrollClass entries | Still disconnects |
| Dynamic screen dimensions in valuators | Still disconnects |
| Fixed master keyboard attachment (4→2) | Still disconnects |
| `sendReplyRaw` combined send (header+payload) | Verified single sendAll() call |

### Key Finding

The `readAndDispatch error` is an **EOF** — the client intentionally closes the connection after receiving our complete, well-formed reply. This is NOT a protocol error on our side. The client parses the reply successfully (the wire format is correct) but then decides to disconnect.

### Likely Root Causes to Investigate

1. **Chromium's post-XIQueryDevice validation**: Read `ui/events/devices/x11/device_data_manager_x11.cc` in Chromium source. It may check for specific conditions after XIQueryDevice (e.g., specific device properties, event mask capabilities, or XI2 event registration requirements) and abort if they're not met.

2. **Multi-process Chromium architecture**: Chromium spawns multiple X11 connections (browser, GPU, renderer). The connection that calls XIQueryDevice may be a helper process (GPU process or utility) that tries XI2, fails an internal check, and exits. The main Vitis window process connects separately and would work fine — which is exactly what we see when XI2 is hidden.

3. **Missing XI2 event registration infrastructure**: After XIQueryDevice, Chromium may try to register for XI2 events (XISelectEvents, XISetClientPointer, etc.) and if those fail or produce unexpected results, it disconnects.

4. **Sequence number interaction**: The `sendAll()` monotonic sequence floor logic in `XProtoTransport.cpp` may be modifying the reply header in edge cases. Check if `max_wire_seq_` is being bumped by interleaved events between the XISelectEvents reply and the XIQueryDevice reply.

### Key Files

- **XI2 handlers**: `ExtensionOps.cpp` lines 1019-1400 (search for `kXInput2`)
- **XIQueryDevice**: `ExtensionOps.cpp` case 48 — builds device list with ButtonClass, ValuatorClass, ScrollClass, KeyClass
- **XInput2Ops.cpp**: Legacy stub dispatcher — currently just logs and skips. Should be removed or merged into ExtensionOps.
- **Transport layer**: `XProtoTransport.cpp` `sendAll()` — monotonic sequence floor, payload tracking
- **Client read loop**: `XProtoDaemon.cpp` `readAndDispatch()` — returns Error on EOF

### Debug Diagnostics Still in Place

- `[XIQueryDevice] seq=N requested_device=N` log line
- Full hex dump of XIQueryDevice reply to stderr
- `SWIFTX11_DEBUG_BUILD` counter for tracking iterations

## Resolved Issues from This Session

### XTEST GrabControl Crash (RESOLVED)
Root cause: `LD_PRELOAD` of `libgdk-x11-2.0.so.0` (GTK2) conflicted with GTK3 loaded by AT-SPI. Removing the GTK2 preload from `start_vivado_sx11.sh` fixed the crash. XTEST v2.2 is now safely enabled.

### JidePopup Decorated Windows (RESOLVED)
Implemented `_MOTIF_WM_HINTS` parsing in `PropOps.cpp`. When `flags & MWM_HINTS_DECORATIONS` and `decorations == 0`, window becomes borderless + floating. JidePopup windows now appear correctly.

### Dialog Window Management (RESOLVED)
- `WM_TRANSIENT_FOR` property parsed and stored
- Transient dialogs shown via `NSApp.activate` + `makeKeyAndOrderFront` (decorated) or `orderFront` (borderless popups)
- `addChildWindow` approach abandoned (caused grey persistent windows)
- `acceptsFirstMouse` on both X11View and X11MTKView for click-through

### Stale Event Delivery on fd Reuse (RESOLVED)
When client 1 disconnects and client 2 reuses the same fd, queued host commands (Key, Button, etc.) were delivered to the wrong client. Fix: input events are now skipped when the owning client isn't found, rather than falling back to an arbitrary client.

### Wide Lines + Dashed Lines (RESOLVED)
`drawLine()` in ShapeOps.cpp now supports `line_width > 1` (perpendicular stroke expansion) and `line_style` (OnOffDash, DoubleDash). `CapNotLast` implemented.

### Cursor Font (RESOLVED)
`OpenFont("cursor")` loads `cursor.pcf.gz` from `/opt/X11/share/fonts/misc/`. QueryFont includes `FONT` property.

### Auto-Ungrab on Window Destroy (RESOLVED)
`DestroyWindow` and `DestroySubwindows` now call `removeForWindows()` to clear grabs.

### SO_SNDBUF Increase (RESOLVED)
Server-side TCP send buffer increased to 4MB to reduce backpressure on Docker bridge.

## Other Items for Future Sessions

### Native macOS Title Bar for Undecorated Windows
Windows with `_MOTIF_WM_HINTS decor=0` (Electron/Vitis) currently use `.borderless` + `isMovableByWindowBackground`. Enhancement: add optional native macOS title bar above X11 content. See TODO.md "Future Enhancements" section.

### Ctrl+Click Regression (MEDIUM)
Ctrl+click no longer triggers button 3 in Vivado. Two-finger trackpad works. Regression in v1.17→v1.19.

### Phase 8.5 Graphics — Remaining Items
- FillPoly fill rule (WindingRule not implemented)
- PolyLine join behavior (square joins only)
- CapRound / CapProjecting for wide lines

### BadGC / BadCursor Strict Validation
Reverted to lenient mode — Java AWT calls RecolorCursor/ChangeGC on resources not in our tables. Full resource tracking needed before re-enabling strict checks.

## Docker Container Setup

- **Image**: `x64-linux-dbus` (AlmaLinux 9 + dbus)
- **DISPLAY**: `host.docker.internal:1` (SwiftX11 on TCP port 6001)
- **dbus**: `dbus-launch --sh-syntax` in startup script, DISPLAY must be set BEFORE dbus-launch
- **No GTK2 preload**: Removed `libgdk-x11-2.0.so.0` from LD_PRELOAD (caused GTK2/3 conflict)
- **Vitis `--disable-gpu`**: Injected via entrypoint patching of Vitis wrapper (sed on line 567)
- **Xilinx volume**: `/Xilinx` Docker volume, read-only for `user`. Entrypoint patches are done as root.
- **Launch scripts**: `/Users/lkg/Documents/Vivado/vivado2023/` — `launch_vivado_sx11.sh`, `launch_vitis_sx11.sh`, `start_vivado_sx11.sh`, `start_vitis_sx11.sh`

## Version

Current: **v1.19.35** (debug build counter at 12). Set `SWIFTX11_DEBUG_BUILD` to 0 for release.

## Build

Open `macos/SwiftX11.xcodeproj` in Xcode. Build and run `SwiftX11` target. Test: `DISPLAY=127.0.0.1:1 xterm -sb -rightbar -bc`.

## Full Documentation

- Architecture: `docs/CLAUDE.md`
- Roadmap: `docs/TODO.md`
- README: `docs/README.md`
- License: `docs/LICENSE` (GPL-3.0)
