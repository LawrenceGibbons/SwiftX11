# SwiftX11 — Phase 8 Protocol Hardening Handoff (v1.19.14)

## Context

SwiftX11 is a native macOS X11 protocol server (Swift + C++). It implements 101 core X11 opcodes and 10 extensions. It successfully runs xterm, xeyes, xcalc, xclock, and Xilinx Vivado from an AlmaLinux 9 Docker container.

## Primary Investigation: XTEST GrabControl Crash

### The Problem

Vivado (Java Swing, running in AlmaLinux 9 Docker container) segfaults (signal 11) when SwiftX11 advertises XTEST v2.2. The crash is 100% reproducible and occurs ~58ms after GrabControl is called.

### Crash Sequence (from wire trace)

```
1. JidePopup window created (dialog box for IP customization)
2. QueryExtension("XTEST") × 2 + QueryExtension("XInputExtension")
3. XTEST GetVersion → server replies v2.2 (major=2, minor=2)
4. XTEST GrabControl (minor 3, impervious=true) ← void, no reply needed
5. ~58ms later: Vivado crashes in gdk_display_manager_get_default_display (signal 11)
```

When server reports v2.0 instead, step 4 never happens, and there is no crash.

### What Has Been Ruled Out

| Hypothesis | Test | Result |
|-----------|------|--------|
| AT-SPI / DBus missing | Added dbus to container, removed NO_AT_BRIDGE=1 | Still crashes |
| Malformed XTEST reply | Hex-dumped GetVersion reply, verified against spec | Byte-perfect |
| Sequence desync | Monitored SEQ_FLOOR bumps | None observed |
| Interleaved events after XTEST | Suppressed flushNotifyQueue after XTEST dispatch | Still crashes |
| Missing replies | Ring buffer shows all reply-bearing requests got replies | All accounted for |

### What Is Known

- **GrabControl is a void request** — no reply expected. Our handler silently consumes it.
- **XQuartz handles XTEST 2.2 + GrabControl without issues** — so the protocol itself is fine.
- **The crash is in the client (Vivado/GDK)**, not in SwiftX11 — the server continues running.
- **The client disconnects** (EOF on fd) because it crashed.

### What to Investigate

1. **What does GrabControl actually DO in Xorg/XQuartz?** Read the Xorg server source (`xtest.c` or similar). GrabControl sets a per-client "impervious" flag that makes the client's XTEST-synthesized events bypass active grabs. Our no-op may leave the client in a state where subsequent grab operations behave unexpectedly.

2. **Does the client immediately do something after GrabControl that depends on impervious state?** Look at what Vivado/GDK/Java does after calling XTestGrabControl. Perhaps it calls GrabPointer or GrabKeyboard expecting the grab to succeed because it's impervious, and our grab implementation doesn't handle the impervious flag.

3. **Does the GrabControl → crash path go through AT-SPI even without DBus?** The crash stack is in GDK. AT-SPI might be trying to use XTEST to synthesize events and depending on impervious grabs working.

### Key Files

- **XTEST handler**: `ExtensionOps.cpp` line ~1449 (search for `kXTEST`)
- **GrabControl stub**: `ExtensionOps.cpp` case 3 (currently just `br.skip(br.remaining())`)
- **Grab implementation**: `GrabOps.cpp` — GrabPointer, GrabKeyboard, active grab state
- **GrabTable**: `include/Core/GrabTable.hpp` — passive/active grab storage
- **Wire trace infrastructure**: `XProtoTransport.cpp` (outgoing), `XProtoDaemon.cpp` (incoming)
- **Ring buffer dump**: `XProtoDaemon.cpp` `removeClient()` — dumps last 16 dispatched + last 32 outgoing

### Current Workaround

`ExtensionOps.cpp` line ~1466: `wire::wr16_le(rep.data() + 8, 0)` reports XTEST v2.0. Client never calls GrabControl. No crash.

## Other High-Priority Items

### JidePopup Decorated Windows (HIGH)

Vivado's JidePopup windows (tooltips, dropdowns, context menus) appear as full decorated NSWindows with title bars. Should be borderless/ephemeral. Need to honor `_NET_WM_WINDOW_TYPE`, `_MOTIF_WM_HINTS`, or the title pattern " " / "JidePopup" to suppress decorations.

v1.19.12 fixed the crash when closing these (was force-disconnecting entire client → now sends UnmapNotify + DestroyNotify). But they still shouldn't be visible as decorated windows.

**Code**: `XProtoDaemon.cpp` WindowClose handler (line ~806), `X11WindowController.swift` window creation.

### Ctrl+Click Regression (MEDIUM)

Ctrl+click no longer triggers context menus (button 3) in Vivado. Two-finger trackpad click works as workaround. Regression somewhere in v1.17→v1.19. `X11WindowHost.swift` mouseDown handler sends button 1 + ControlMask (not button 3). Need to compare with working version to find the regression.

### Pointer Coordinate Offset After Left-Edge Resize (MEDIUM)

When resizing a Vivado window by dragging the left edge, menu selections are offset horizontally. Likely stale origin in coordinate transform — ConfigureNotify may not be sent with updated x/y after left-edge resizes.

## Diagnostic Infrastructure

### Wire Trace Toggle
UI toggle "Wire Trace (stderr)" in ContentView.swift. When enabled, every incoming request and outgoing packet is logged to Xcode console via fprintf. Controlled by atomic `g_wire_trace` in SwiftBridge.cpp. Use this to capture the full request/response stream around crash events.

### Ring Buffers
On client disconnect, the last 16 dispatched requests and last 32 outgoing packets are dumped to the SwiftX11 log window at verbosity level 1.

### Docker Container Setup
- **Old image** (`x64-linux`): No dbus, NO_AT_BRIDGE=1, no entrypoint
- **New image** (`x64-linux-dbus`): dbus running, NO_AT_BRIDGE removed, entrypoint starts system bus
- Both use: `DISPLAY=host.docker.internal:1`, TCP connection to SwiftX11
- Launch script: `start_vivado_sx11.sh` in `/home/user/`
- Docker files: `/Users/lkg/Documents/Vivado/vivado2023/`

## Version

Current: **v1.19.14** — XTEST v2.0 (safe mode), JidePopup close fix, CompareCursor stub, wire trace toggle.

## Build

Open `macos/SwiftX11.xcodeproj` in Xcode. Build and run `SwiftX11` target. Test: `DISPLAY=127.0.0.1:1 xterm -sb -rightbar -bc`.

## Full Documentation

- Architecture: `docs/CLAUDE.md`
- Roadmap: `docs/TODO.md`
- Previous session transcript: check `.claude/` directory for `.jsonl` files
