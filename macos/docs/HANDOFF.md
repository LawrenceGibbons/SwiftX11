# SwiftX11 Session Handoff

Last updated: 2026-03-17 (v1.15.0)

## Current State

SwiftX11 is a working X11 server for macOS. xterm, xcalc, xeyes all work. XI2 (XInput2) event delivery infrastructure is built but has a client-side compatibility issue with xeyes that needs debugging.

**Branch**: `develop++` — all work is on this branch
**Version**: 1.15.0 (defined in `X11LowLevel/include/SwiftX11Version.h`)
**Bundle ID**: `com.rlan.SwiftX11`

## URGENT: XI2 + xeyes Black Eyes Bug

### The Problem
When XInputExtension is advertised (`present=1`), xeyes shows solid black ovals instead of white eyes with pupils. Disabling XI2 (`present=0` in QueryOps.cpp line ~620) makes xeyes work perfectly. **XQuartz handles this correctly** — xeyes works fine there with XI2 advertised.

### What's Been Verified (server side is correct)
- All XI2 reply wire formats are correct (verified with byte-level `[WIRE_HDR]` dumps)
- Expose events ARE delivered to the socket (`sent=1` for both host and child windows)
- Surface IS registered (`hasSurface=1, surfWH=150x100`) when sendExposeSubtree runs
- xeyes sends only 4 XI2 opcodes: minor=1 (GetExtensionVersion ×2), minor=47 (XIQueryVersion), minor=46 (XISelectEvents with XI_RawMotionMask=0x00020000 on root)
- No XCB sequence desync — all reply sizes are correct, monotonic floor is intact

### What's Built (all compiled and wired in)
- `XI2EventMask.hpp`: Event type constants, mask bits (including Raw events)
- `WindowState/WindowView.xi2_mask`: Per-window XI2 event selection
- `InputState.xi2_root_mask`: Root window XI2 selections (for global tracking apps)
- `XProtoTransport::sendEventVariable()`: Variable-length event sender
- `EventOps`: 6 XI2 senders (Motion, Button, Key, Crossing, Focus, RawMotion)
- `ExtensionOps`: XISelectEvents parser stores mask; root window handled via InputState
- 13 injection points sending XI2 events alongside core events
- XInputExtension advertised in QueryExtension + ListExtensions (nExt=10)

### Recommended Next Steps
1. **Capture XQuartz wire traffic with Xscope**: `xscope -display :0 -port 6010` then `DISPLAY=:10 xeyes`. Compare event sequences between XQuartz and SwiftX11.
2. **Check Xlib/libXi source for `_XiCheckExtInit`**: This is where Xlib hooks into event processing when XI2 is detected. Understanding what it changes will pinpoint the issue.
3. **Try without GE extension**: If disabling "Generic Event Extension" (`present=0` for GE in QueryOps) also fixes xeyes, the issue may be in GE's event cookie handling, not XI2 specifically.

### Debug Traces Still Active (#ifndef NDEBUG)
- `[XI2_OP]`, `[XI2_MASK_ROOT]` — XI2 opcode/mask tracking
- `[WIRE_HDR]` — raw reply/event headers (first 80 seqs)
- `[DRAIN_HOST]` — host command processing
- `[SET_PRESENTABLE_DBG]`, `[EXPOSE_SUBTREE_DBG]`, `[EXPOSE_NOW_DBG]` — Expose delivery
- `[VIEW_MOVE_WIN]`, `[ATTACH_SETTLE]`, `[ENSURE_SURFACE]` — Swift surface lifecycle
- `[SHAPE_DBG]` — layer hierarchy dump
These should be cleaned up after the XI2 issue is resolved.

## Other Changes This Session (v1.15.0)

### WM Size Floor Fix
- Lowered WM floor threshold from `< 200 || < 100` to `< 10` pixels
- Prevents legitimate small windows (xeyes 150×100) from being enlarged

### Shape AA Compositing (reverted, TODO)
- 3×3 box-filter AA was implemented in `applyShapeMask()` but reverted
- Metal pipeline uses straight alpha (`sourceAlpha/oneMinusSourceAlpha`), not premultiplied
- Re-apply with straight alpha once xeyes works with XI2

### Release Linker Fixes
- 8 extension stub .cpp files had incomplete `ByteReader` forward declarations
- Fixed by replacing with `#include "Utils/ByteReader.hpp"`
- `GCC_INLINES_ARE_PRIVATE_EXTERN=YES` in Release made out-of-line copies hidden

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
| `include/Core/XI2EventMask.hpp` | NEW: event types, masks, wire sizes |
| `include/Core/WindowTable.hpp` | xi2_mask in WindowState |
| `include/Core/WindowView.hpp` | xi2_mask field |
| `include/Core/InputState.hpp` | xi2_root_mask |
| `include/Ops/EventOps.hpp` | 6 XI2 event sender declarations |
| `include/Transport/XProtoTransport.hpp` | sendEventVariable() |
| `src/Ops/EventOps.cpp` | XI2 event builders + senders |
| `src/Ops/ExtensionOps.cpp` | XISelectEvents parser, XI2 opcode trace |
| `src/Transport/XProtoTransport.cpp` | sendEventVariable(), wire header dump |
| `src/Transport/XProtoDaemon.cpp` | Host command drain diagnostics |
| `src/XProtoServerBridge.cpp` | XI2 injection points, expose diagnostics |
| `src/XProtoNotifyBridge.cpp` | XI2 injection points |
| `src/Ops/QueryOps.cpp` | XI2 advertisement (present=1, nExt=10) |
| `src/Core/WindowTable.cpp` | setXI2Mask(), snapshot xi2_mask |
| `SwiftX11/UI/Windows/X11WindowHost.swift` | Surface lifecycle diagnostics |
| `X11LowLevel/include/SwiftX11Version.h` | v1.15.0 |

## Priority for Next Session
1. **Fix XI2 + xeyes** (compare with XQuartz using Xscope)
2. **Shape AA compositing** (straight alpha, re-apply once xeyes works)
3. **Clean up debug traces**
4. **Build Release + .pkg installer**
5. **Update CLAUDE.md** with XI2 architecture details
