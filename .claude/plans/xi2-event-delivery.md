# XI2 Event Delivery Implementation Plan

## Problem
When XInputExtension is advertised, Xlib apps (xeyes, etc.) call XISelectEvents to register for XI2 input events instead of core events. Currently XISelectEvents is silently consumed but XI2 events are never delivered. Apps that switch to XI2 mode get no input events and appear dead (black/unresponsive).

## Solution
Implement XI2 event delivery: parse+store XISelectEvents masks, build XI2 GenericEvents, send them alongside core events at all existing delivery points.

## Wire Formats (from XI2proto.h on this system)

### xXIDeviceEvent (types 2-6: KeyPress/Release, ButtonPress/Release, Motion)
Fixed part = 80 bytes. With buttons_len=1, valuators_len=0: **84 bytes total**, length=**13**.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | type = 35 (GenericEvent) |
| 1 | 1 | extension = 141 (kXInput2) |
| 2 | 2 | sequenceNumber |
| 4 | 4 | length = 13 (words after 32-byte header) |
| 8 | 2 | evtype (2=KeyPress..6=Motion) |
| 10 | 2 | deviceid (2=pointer, 3=keyboard) |
| 12 | 4 | time (ms) |
| 16 | 4 | detail (button/keycode, 0 for motion) |
| 20 | 4 | root window |
| 24 | 4 | event window |
| 28 | 4 | child window |
| 32 | 4 | root_x (FP16.16) |
| 36 | 4 | root_y (FP16.16) |
| 40 | 4 | event_x (FP16.16) |
| 44 | 4 | event_y (FP16.16) |
| 48 | 2 | buttons_len = 1 |
| 50 | 2 | valuators_len = 0 |
| 52 | 2 | sourceid |
| 54 | 2 | pad |
| 56 | 4 | flags = 0 |
| 60 | 16 | mods (base, latched, locked, effective — 4×u32) |
| 76 | 4 | group (base, latched, locked, effective — 4×u8) |
| 80 | 4 | button_mask (1 word, buttons_len=1) |

### xXIEnterEvent (types 7-10: Enter, Leave, FocusIn, FocusOut)
Fixed part = 72 bytes. With buttons_len=1: **76 bytes total**, length=**11**.

| Offset | Size | Field |
|--------|------|-------|
| 0-7 | 8 | GenericEvent header (type=35, ext=141, seq, length=11) |
| 8 | 2 | evtype (7=Enter..10=FocusOut) |
| 10 | 2 | deviceid |
| 12 | 4 | time (ms) |
| 16 | 2 | sourceid |
| 18 | 1 | mode (0=Normal, 1=Grab, 3=WhileGrabbed) |
| 19 | 1 | detail (0=Ancestor, 1=Virtual, etc.) |
| 20 | 4 | root window |
| 24 | 4 | event window |
| 28 | 4 | child window |
| 32 | 4 | root_x (FP16.16) |
| 36 | 4 | root_y (FP16.16) |
| 40 | 4 | event_x (FP16.16) |
| 44 | 4 | event_y (FP16.16) |
| 48 | 1 | same_screen (BOOL) |
| 49 | 1 | focus (BOOL) |
| 50 | 2 | buttons_len = 1 |
| 52 | 16 | mods (4×u32) |
| 68 | 4 | group (4×u8) |
| 72 | 4 | button_mask (1 word) |

## Implementation Steps

### Step 1: Add XI2 mask storage to WindowState
**File: `WindowTable.hpp`**
- Add `uint32_t xi2_mask = 0;` to `WindowState` (next to `event_mask`)
- Add `uint32_t xi2_mask = 0;` to `WindowView`
- Add `void setXI2Mask(uint32_t xid, uint32_t mask);` method
- Copy `xi2_mask` in `snapshot()`

### Step 2: Add XI2 event mask constants
**New file: `include/Core/XI2EventMask.hpp`**
```cpp
namespace x11::xi2 {
  constexpr uint32_t kKeyPressMask      = (1 << 2);
  constexpr uint32_t kKeyReleaseMask    = (1 << 3);
  constexpr uint32_t kButtonPressMask   = (1 << 4);
  constexpr uint32_t kButtonReleaseMask = (1 << 5);
  constexpr uint32_t kMotionMask        = (1 << 6);
  constexpr uint32_t kEnterMask         = (1 << 7);
  constexpr uint32_t kLeaveMask         = (1 << 8);
  constexpr uint32_t kFocusInMask       = (1 << 9);
  constexpr uint32_t kFocusOutMask      = (1 << 10);
  // Event types
  constexpr uint16_t kKeyPress       = 2;
  constexpr uint16_t kKeyRelease     = 3;
  constexpr uint16_t kButtonPress    = 4;
  constexpr uint16_t kButtonRelease  = 5;
  constexpr uint16_t kMotion         = 6;
  constexpr uint16_t kEnter          = 7;
  constexpr uint16_t kLeave          = 8;
  constexpr uint16_t kFocusIn        = 9;
  constexpr uint16_t kFocusOut       = 10;
  // Device IDs
  constexpr uint16_t kVirtualCorePointer  = 2;
  constexpr uint16_t kVirtualCoreKeyboard = 3;
}
```

### Step 3: Add `sendEventVariable()` to XProtoTransport
**File: `XProtoTransport.hpp/.cpp`**
Mirrors `sendEvent32` but accepts variable-length data. Same owner_fd check, uses `sendAll()` for the actual send.

### Step 4: Add XI2 event builders and senders to EventOps
**Files: `EventOps.hpp`, `EventOps.cpp`**

New methods (all check `wv->xi2_mask` internally, no-op if mask bit not set):
- `sendXI2MotionEvent(ctx, wid, root_x, root_y, buttons, mods)`
- `sendXI2ButtonEvent(ctx, wid, is_press, button, root_x, root_y, buttons, mods, child)`
- `sendXI2KeyEvent(ctx, wid, is_press, keycode, buttons, mods)`
- `sendXI2CrossingEvent(ctx, wid, is_enter, root_x, root_y, buttons, mods)`
- `sendXI2FocusEvent(ctx, wid, is_in)`

Each builds the appropriate wire format and sends via `sendEventVariable()`.

### Step 5: Parse XISelectEvents (minor 46) properly
**File: `ExtensionOps.cpp`**
Replace `br.skip(br.remaining())` with actual parsing of window, num_masks, deviceid, mask_len, mask data. Store combined mask via `ctx.windows().setXI2Mask(window, mask)`.

### Step 6: Update XIGetSelectedEvents (minor 60)
**File: `ExtensionOps.cpp`**
Return stored XI2 mask instead of empty (num_masks=0).

### Step 7: Inject XI2 sends at all event delivery sites
Add one XI2 send call after each existing core event send:

**XProtoNotifyBridge.cpp** (~3 sites):
- Line 133: after `sendCrossingEvent(leave)` → add `sendXI2CrossingEvent(leave)`
- Line 136: after `sendCrossingEvent(enter)` → add `sendXI2CrossingEvent(enter)`
- Line 230: after `sendMotionNotify` → add `sendXI2MotionEvent`

**XProtoServerBridge.cpp** (~10 sites):
- Line 572: after `sendCrossingEvent(enter)` → add `sendXI2CrossingEvent`
- Line 597: after `sendCrossingEvent(leave)` → add `sendXI2CrossingEvent`
- Line 625: after `sendFocusEventDirect(out)` → add `sendXI2FocusEvent`
- Line 650: after `sendFocusEventDirect(in)` → add `sendXI2FocusEvent`
- Line 660: after `sendFocusEventDirect(out)` → add `sendXI2FocusEvent`
- Line 877: after `sendButtonEvent` → add `sendXI2ButtonEvent`
- Line 934: after `sendButtonEvent` → add `sendXI2ButtonEvent`
- Line 941: after `sendButtonEvent` → add `sendXI2ButtonEvent`
- Line 1020: after `sendKeyEvent` → add `sendXI2KeyEvent`

### Step 8: Re-enable XInputExtension advertisement
**File: `QueryOps.cpp`**
- `handleQueryExtension`: set `present=1; major=ext::kXInput2;` for "XInputExtension"
- `handleListExtensions`: add "XInputExtension" to the list

### Step 9: Version bump + CLAUDE.md update

## Files Changed (10)
1. `include/Core/XI2EventMask.hpp` — NEW: constants
2. `include/Core/WindowTable.hpp` — add xi2_mask to WindowState/WindowView
3. `src/Core/WindowTable.cpp` — setXI2Mask(), snapshot copy
4. `include/Transport/XProtoTransport.hpp` — sendEventVariable() decl
5. `src/Transport/XProtoTransport.cpp` — sendEventVariable() impl
6. `include/Ops/EventOps.hpp` — XI2 sender declarations
7. `src/Ops/EventOps.cpp` — XI2 event builders + senders
8. `src/Ops/ExtensionOps.cpp` — parse XISelectEvents, update XIGetSelectedEvents
9. `src/XProtoNotifyBridge.cpp` — 3 XI2 injection points
10. `src/XProtoServerBridge.cpp` — ~10 XI2 injection points
11. `src/Ops/QueryOps.cpp` — re-enable advertisement

## Testing
- xeyes: should show white eyes with pupils tracking mouse (not black)
- xterm: should still work (xterm doesn't use XI2)
- xcalc: should still work
