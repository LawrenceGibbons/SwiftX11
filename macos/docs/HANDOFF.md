# SwiftX11 Session Handoff

Last updated: 2026-03-18 (v1.15.12)

## Current State

SwiftX11 is a working X11 server for macOS. xterm, xcalc, xeyes, Vivado all work. Version 1.15.12 on `develop++` branch.

**Bundle ID**: `com.rlan.SwiftX11`

## Session Accomplishments (v1.15.0 → v1.15.12)

### Completed
1. **XI2 xeyes black eyes fix** (v1.15.4): `first_event=93` prevents libXi from overwriting core event handlers. 68-byte RawMotion with valuator data.
2. **Global RawMotion delivery** (v1.15.3): `GlobalPointerTracker` (NSEvent global+local monitor) for xeyes pupil tracking outside X11 windows.
3. **Shape AA compositing** (v1.15.5): 3×3 box-filter with straight alpha (not premultiplied). Cardinal-neighbor fast-path for interior pixels.
4. **Debug trace cleanup** (v1.15.7): ~50 `#ifndef NDEBUG` fprintf removed across 15+ files.
5. **Vivado banner race fix** (v1.15.10): Deferred show waits for first present (not floor-size check). WM floor 200×100 can match banner's real size.
6. **Clipboard improvements** (v1.15.8-9): Removed proactive ClipboardCapture (deadlocked same-client). Same-client ConvertSelection returns property=None. NSPasteboard direct access (no DispatchQueue.main).
7. **Socket backpressure** (v1.15.12): 1MB SO_SNDBUF, PointerMove throttling via poll(POLLOUT), EAGAIN poll-wait instead of tight spin.
8. **SEQ_REGRESS false alarm fix**: Per-transport dbg_last_sent_seq_ replaces leaked thread_local.
9. **TODO.md updated**: XI2, XTEST, Shape AA sections current. v1.15.x status documented.
10. **CLAUDE.md updated**: XI2 architecture, GlobalPointerTracker, new key files.

## URGENT: Vivado Edit → Copy Hang

### The Problem
When clicking Edit → Copy in Vivado's menu (after selecting text in the IP status report), Vivado hangs with the Edit menu still displayed. Also happens when selecting text in the TCL console.

### What's Been Proven
- **NOT socket backpressure**: No `[BACKPRESSURE]` or `[THROTTLE]` traces. 1MB SO_SNDBUF + PointerMove throttling confirmed working.
- **NOT selection protocol**: No `[SEL]` traces appear — Java never sends SetSelectionOwner or ConvertSelection. The hang is BEFORE any X11 selection request reaches the wire.
- **NOT clipboard bridge**: NSPasteboard callbacks work (direct access, no main-thread dispatch).
- **NOT ClipboardCapture**: Proactive clipboard capture removed entirely. Same-client ConvertSelection returns None.
- **Server is responsive**: xproto thread continues processing other events normally. No stuck writes.

### Working Theory
Java's Copy action handler on the EDT calls `XSync()` (which sends `GetInputFocus` and waits for the reply). During the `_XReply()` wait, Xlib reads events from the socket. Something about how we deliver events during that read causes Java's event processing to deadlock internally.

### Recommended Next Steps
1. **Add trace to GetInputFocus** (opcode 43): Log when it's received and when the reply is sent. If Java calls GetInputFocus during Copy and we don't respond, that's the hang.
2. **Wire capture with xscope**: `xscope -display :1 -port 6010` then `DISPLAY=:10 vivado`. Compare with XQuartz wire traffic.
3. **Check if menus work with XQuartz**: If Vivado Copy works with XQuartz, compare what XQuartz does differently (it may have special clipboard handling in its XSync).
4. **Test with a simple Java app**: Write a minimal Java Swing app with a JTextArea and Edit→Copy to isolate whether it's Vivado-specific or all Java Swing.

### Debug Traces Still Active
- `[BACKPRESSURE]` — sendAll EAGAIN detection (fires only on backpressure)
- `[THROTTLE]` — PointerMove skip count (fires only on backpressure)
- `[SEL]` — SetSelectionOwner and ConvertSelection (fires on every call)
- `[WM_HINTS_DBG]` — WM_NORMAL_HINTS parser (fires on every WM_NORMAL_HINTS)
- `[MAP_SHOW]` — mapWindow geometry diagnostic (fires on every non-OR map)
- `[DEFER_SHOW]` — deferred show retry loop (fires only for floor-sized windows)

These should be cleaned up once the clipboard issue is resolved.

## Other Known Issues (v1.15.12)
- **Vivado startup banner**: Sometimes shows at 200×100 (when ConfigureWindow arrives after MapWindow). Fixed in v1.15.10 with present-on-first-draw; needs more testing.
- **xeyes shaped window black flash on resize**: Minor cosmetic issue during live resize.

## Build & Run

```bash
cd /Users/lkg/Documents/Vivado/SwiftX11/macos
xcodebuild -project SwiftX11.xcodeproj -scheme SwiftX11 -configuration Debug build

# Test clients
DISPLAY=127.0.0.1:1 xeyes
DISPLAY=127.0.0.1:1 xterm -sb -rightbar -bc
DISPLAY=127.0.0.1:1 xcalc
```

## Key Files Modified This Session

| File | Change |
|------|--------|
| `XProtoTransport.cpp` | EAGAIN poll-wait, backpressure trace, per-transport seq regression detector |
| `XProtoDaemon.cpp` | 1MB SO_SNDBUF, PointerMove throttle, ClipboardCapture no-op |
| `SelectionOps.cpp` | Removed proactive ClipboardCapture, same-client ConvertSelection guard, [SEL] traces |
| `XServerController.swift` | NSPasteboard direct access (no DispatchQueue.main) |
| `WindowRegistry.swift` | Banner deferred show rework, present-path show on first draw |
| `X11WindowHost.swift` | Shape AA with straight alpha (3×3 box filter) |
| `PropOps.cpp` | [WM_HINTS_DBG] trace |
| `WindowTable.cpp` | Removed XI2_MASK trace |
| `EventOps.cpp` | Removed FOCUS_DIRECT, BTN_EVENT traces |
| `~50 files` | Debug trace cleanup (removed ~50 #ifndef NDEBUG fprintf blocks) |
| `CLAUDE.md` | XI2 architecture, GlobalPointerTracker, updated Current State |
| `TODO.md` | XI2 events done, XTEST updated, v1.15.x status |
| `SwiftX11Version.h` | v1.15.12 |

## Priority for Next Session
1. **Fix Vivado Edit→Copy hang** (wire-level investigation needed)
2. **Clean up diagnostic traces** ([SEL], [MAP_SHOW], [WM_HINTS_DBG], etc.)
3. **Update CLAUDE.md** with clipboard/backpressure architecture
4. **Vitis testing** (Phase 8)
