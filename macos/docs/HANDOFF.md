# SwiftX11 — Phase 10 Handoff (v1.19.35.26-dbg)

## Context

SwiftX11 is a native macOS X11 protocol server (Swift + C++). It implements 101 core X11 opcodes and 12 extensions. It successfully runs xterm, xeyes, xcalc, xclock, xfd, Xilinx Vivado 2025.1.1 (Java Swing), and Xilinx Vitis 2025.1.1 (Electron/Theia) from an AlmaLinux 9 Docker container.

**Vitis status**: Main window renders with native macOS title bar. HTML menus highlight on hover but dropdowns don't fire (Electron-internal JS/CSS issue — menus are NOT X11 popup windows). Portal-GTK dialogs ("Open Folder") now render with mostly correct colors (Composite/DAMAGE stubs added) but remain non-interactive. Cross-client event routing functional with sequence restamping.

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

### 1. Vitis Menu Dropdowns Don't Fire (HIGH — was working in earlier server version)

Electron's HTML menu bar highlights items on hover, but clicking does NOT trigger the dropdown. **Key finding (v1.19.35.25)**: `[CREATE_OR]` tracing confirmed Electron creates ZERO override-redirect popup windows for menus. The menus are rendered entirely inline via HTML/CSS/JS within the main Electron window. The dropdown failure is an Electron-internal issue triggered by something in SwiftX11's input event handling.

**The user reports this was working in a previous server version** (quite a ways back). This means a change in input event delivery, focus management, or grab behavior broke the click handler that triggers dropdowns.

**Investigation approach**:
- Enable wire trace (`x11_set_wire_trace`) and click a Vitis menu item. Compare the ButtonPress/ButtonRelease/MotionNotify events delivered to Electron vs what works on XQuartz.
- Check if a grab (GrabPointer/GrabKeyboard) is active that's eating the button event before Electron's JS sees it.
- Check if FocusOut events are being sent that cause Electron to dismiss the menu before it opens.
- Check if ButtonRelease is missing or has wrong coordinates (Electron might need Press+Release in same window to register a click).
- Look at the event_mask on the Vitis main window and its child windows — if ButtonPressMask isn't set, events won't be delivered.

**Key files**: `XProtoServerBridge.cpp` (button handler), `GrabOps.cpp`, `InputRouting.cpp`, `EventOps.cpp`

**Diagnostic**: `[CREATE_OR]` trace is already in place — if a fix causes Electron to start creating popup windows, we'll see them in stderr.

### 2. Portal-GTK Dialog Rendering & Interaction (MEDIUM)

The xdg-desktop-portal-gtk process creates "Set Workspace" / "Open Folder" dialogs. With Composite/DAMAGE stubs (v1.19.35.25), dialogs now render with mostly correct layout and readable text (sidebar, file list, buttons all visible). Screenshot shows significant improvement over fully-inverted rendering.

**Remaining issues**:
- **Some color oddities**: Widgets not fully correct — possibly ARGB32 alpha channel handling. GTK3 creates depth-32 windows for transparency; verify `CreateWindow` handles depth-32 and alpha isn't inverted in the rendering pipeline.
- **Non-interactive**: Clicks don't register on sidebar items, file list, or Cancel/Open buttons. Verify portal-gtk child windows receive ButtonPress events with correct coordinates. Cross-client event routing works (v1.19.35.23), but portal-gtk creates many small child windows that need precise coordinate hit-testing.
- **Tooltip stacking**: OR popup windows from portal-gtk appear behind the dialog. Check that OR windows from portal-gtk get `.floating` level above the dialog's `.normal` level.

### 3. Ctrl+Click Regression (MEDIUM)

Ctrl+click no longer triggers button 3 in Vivado. Two-finger trackpad works. Regression in v1.17→v1.19.

### 4. XI2 XIQueryDevice Chromium Disconnect (LOW — workaround in place)

Chromium/Electron disconnects after receiving XIQueryDevice reply. Workaround: hide XInputExtension (`present=0` in QueryExtension). Vitis works without XI2.

## What Was Fixed This Session (v1.19.35.23 → v1.19.35.25)

| # | Fix | Description |
|---|-----|-------------|
| 1 | Cross-client seq fix | `sendEventCrossClient` restamps event seq with target transport's `lastSeq()` — fixes XCB "Unknown sequence number" crash |
| 2 | Composite extension | v0.4 stub (major 143) — portal-GTK dialogs render with mostly correct colors |
| 3 | DAMAGE extension | v1.1 stub (major 144) — paired with Composite for GTK3 compositor awareness |
| 4 | OR popup tracing | `[CREATE_OR]` logs every override-redirect window creation with XID, position, size, fd |
| 5 | Vitis menu diagnosis | Confirmed menus are inline HTML (not OR popups) — dropdown failure is Electron-internal |

**What was investigated but NOT the cause**: `_NET_FRAME_EXTENTS` (0,0,0,0) — tried for MOTIF decor=0 windows, didn't fix menus (because menus aren't X11 popups), reverted to top=28.

## Resolved Issues from Previous Sessions

### Cross-Client Event Seq Poisoning (RESOLVED v1.19.35.24)
`sendEventCrossClient()` sent events with source client's sequence, poisoning target's `max_wire_seq_`. Diagnosed from wire ring: fd=7 entry had seq=181 (fd=12's sequence). Fix: restamp bytes[2:3] with target's `lastSeq()`.

### XTEST GrabControl Crash (RESOLVED)
Root cause: `LD_PRELOAD` of `libgdk-x11-2.0.so.0` (GTK2) conflicted with GTK3 loaded by AT-SPI.

### JidePopup Decorated Windows (RESOLVED)
`_MOTIF_WM_HINTS` parsing. `decor=0` → borderless + floating for popups, titled for main windows.

### Dialog Window Management (RESOLVED)
`WM_TRANSIENT_FOR` parsed, transient dialogs shown via `makeKeyAndOrderFront`, `addChildWindow` abandoned.

### Stale Event Delivery on fd Reuse (RESOLVED)
Input events skipped when owning client isn't found.

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

Current: **v1.19.35.26-dbg** (debug build counter at 26). Set `SWIFTX11_DEBUG_BUILD` to 0 for release.

## Build

Open `macos/SwiftX11.xcodeproj` in Xcode. Build and run `SwiftX11` target. Test: `DISPLAY=127.0.0.1:1 xterm -sb -rightbar -bc`.

## Full Documentation

- Architecture: `docs/CLAUDE.md`
- Roadmap: `docs/TODO.md`
- README: `docs/README.md`
- License: `docs/LICENSE` (GPL-3.0)
