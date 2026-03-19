# SwiftX11 — Phase 8 Protocol Hardening Handoff

## Context

SwiftX11 is a native macOS X11 protocol server (Swift + C++). It implements 101 core X11 opcodes and 10 extensions. It successfully runs xterm, xeyes, xcalc, xclock, and Xilinx Vivado.

Several bugs in v1.15–v1.17 stemmed from simplified handler implementations that didn't fully respect the X11 protocol spec:

| Bug | Root cause | Version fixed |
|-----|-----------|---------------|
| Vivado Edit→Copy hang | `ChangeProperty` didn't send `PropertyNotify` | v1.15.18 |
| Vivado Edit→Copy hang | `UngrabKeyboard` didn't send `FocusIn`/`FocusOut` | v1.15.17 |
| Vivado banner wrong size | `kWM_NORMAL_HINTS` atom constant was 41 instead of 40 | v1.17.0 |
| Vivado banner race | `MapWindow` before `ConfigureWindow` — no SubstructureRedirect | v1.17.0 |
| CreateWindow size floor | Server-imposed 200×100 minimum didn't match WM behavior | v1.17.1 |

The goal of Phase 8 is a systematic audit of all handlers against the X11 protocol specification, fixing compliance gaps before they become runtime bugs.

## Architecture Quick Reference

- **Swift** (`macos/SwiftX11/`): AppKit windows, Metal rendering, input events, networking
- **C++** (`macos/X11LowLevel/cpp/X11Protocol/`): Wire protocol parsing, request dispatch, raster drawing, resource tables
- **Bridge**: `SwiftBridge.cpp` — `extern "C"` functions connecting Swift ↔ C++
- **Dispatch**: `XProtoServer::dispatch()` in `src/Core/XProtoServer.cpp` — 256-entry opcode table
- **Handlers**: `src/Ops/*.cpp` — 20 files, ~13,700 lines

See `macos/docs/CLAUDE.md` for full architecture documentation.

## What to Do

The full audit plan is in `macos/docs/TODO.md` under **Phase 8: X11 Protocol Hardening**. It has 10 sub-sections (8.1–8.10), ordered by priority:

### Priority order

1. **8.1 Event Delivery Audit** (HIGH) — The most impactful category. Missing events cause client hangs (Java AWT), widget misbehavior, and toolkit assertion failures. Key items:
   - `PropertyNotify` on all property mutations (partially done, verify completeness)
   - `ConfigureNotify` with correct fields on all geometry changes
   - `FocusIn`/`FocusOut` with correct `mode`/`detail` on all focus transitions
   - `GraphicsExposure`/`NoExposure` for `CopyArea`/`CopyPlane`
   - `SelectionClear` when selection owner changes

2. **8.2 Error Generation Audit** (HIGH) — Missing errors cause silent corruption and confusing client behavior. Systematic `BadWindow`/`BadDrawable`/`BadGC` validation across all handlers.

3. **8.4 Window Operations Audit** (HIGH) — `CreateWindow`, `ConfigureWindow`, `DestroyWindow`, `ReparentWindow` edge cases. `CirculateWindow` (opcode 13) is unimplemented.

4. **8.6 Grab Semantics Audit** (MEDIUM) — `AllowEvents`, `confine_to`, grab freezing, auto-ungrab on destroy. Currently most grab semantics are simplified.

5. **8.3 Reply Format Audit** (MEDIUM) — Verify reply byte layouts match spec.

6. **8.5 Graphics Operations Audit** (MEDIUM) — `subwindow_mode`, `GraphicsExposure`, wide lines, dash patterns.

7. **8.8 Selection Protocol Audit** (MEDIUM) — `INCR` protocol, `MULTIPLE` target, timestamp validation.

8. **8.10 Extension Protocol Audit** (MEDIUM) — RENDER clipping, XFIXES regions, XI2 event fields.

9. **8.7 Atom and Property Audit** (LOW) — `RotateProperties`, `GetProperty` delete semantics.

10. **8.9 Colormap Audit** (LOW) — Minimal impact for TrueColor-only server.

### How to work

1. **Read the spec** for each handler being audited:
   - Core protocol: https://www.x.org/releases/current/doc/xproto/x11protocol.html
   - ICCCM: https://www.x.org/releases/current/doc/icccm/icccm.html

2. **Read the handler code** in `src/Ops/*.cpp` — compare against spec requirements

3. **Fix gaps** — add missing events, errors, field corrections

4. **Test with `xev`** — `xev` shows every event the server sends; use it to verify correct event delivery after changes

5. **Regression test with Vivado** — run Vivado after each batch of fixes

6. **Bump version** in `macos/X11LowLevel/include/SwiftX11Version.h` after each change

### Key handler files

| File | Opcodes | Priority items |
|------|---------|----------------|
| `PropOps.cpp` | 18-21, 114 | PropertyNotify completeness |
| `WindowOps.cpp` | 1, 4-5, 7-12 | CreateNotify, DestroyNotify, MapNotify, UnmapNotify, CirculateWindow |
| `WindowAttrOps.cpp` | 2-3, 12, 14 | ConfigureNotify fields, GetWindowAttributes reply |
| `GrabOps.cpp` | 26-37 | Focus events on grab/ungrab, AllowEvents, confine_to |
| `SelectionOps.cpp` | 22-25 | SelectionClear, timestamp validation, INCR |
| `DrawOps.cpp` | 61-63, 72-77 | GraphicsExposure, subwindow_mode |
| `QueryOps.cpp` | 14-15, 38-44, 91, 98-99, 101, 133 | Reply format verification |
| `EventOps.cpp` | (events) | Crossing event mode/detail, focus event mode/detail |
| `ColorOps.cpp` | 78-92 | BadAccess for read-only colormaps |
| `MiscOps.cpp` | 100-115, 127 | Stubs that may need real implementation |

## Version

Current version: **v1.17.1** (`macos/X11LowLevel/include/SwiftX11Version.h`)

## Build

Open `macos/SwiftX11.xcodeproj` in Xcode. Build and run the `SwiftX11` target. Test with `DISPLAY=127.0.0.1:1 xterm -sb -rightbar -bc`.
