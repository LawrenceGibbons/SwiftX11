# SwiftX11 Session Handoff

Last updated: 2026-03-18 (v1.15.22)

## Current State

SwiftX11 is a working X11 server for macOS. xterm, xcalc, xeyes, Vivado all work. Version 1.15.22 on `develop++` branch.

**Bundle ID**: `com.rlan.SwiftX11`

## Session Accomplishments (v1.15.15 → v1.15.22)

### Completed
1. **Vivado Edit→Copy hang fixed** (v1.15.17): Root cause was two missing X11 spec events:
   - **FocusIn/FocusOut with mode=Ungrab** after UngrabKeyboard — Java AWT's focus state machine required these to proceed past the menu ungrab. Discovered via xscope comparison with XQuartz.
   - **PropertyNotify after ChangeProperty** (v1.15.18) — Java writes `_SUNW_JAVA_AWT_TIME` via ChangeProperty(Append) and waits for PropertyNotify to extract the server timestamp for SetSelectionOwner(CLIPBOARD, time). Without it, Java blocked after InternAtom("CLIPBOARD").
2. **Bidirectional clipboard bridge** (v1.15.19-20):
   - Proactive SelectionRequest on SetSelectionOwner: server sends SelectionRequest(UTF8_STRING) to the new owner, intercepts the SelectionNotify response via handleSendEvent, and pushes the content to NSPasteboard.
   - Works for both CLIPBOARD (Vivado Edit→Copy) and PRIMARY (xterm select-to-copy).
   - Root proxy ownership (`sSelOwner[sel] = 1`) assigned after capture so subsequent ConvertSelection from other X11 clients routes through server.
3. **PropertyNotify event mask filtering** (v1.15.22): PropertyNotify now only sent to windows with PropertyChangeMask selected. Previously sent unconditionally, flooding Java with hundreds of type=28 events during Vivado startup.
4. **MotionNotify coalescing** (v1.15.15): HostCommandQueue coalesces consecutive same-window PointerMove commands at push time. Prevents AWT lock starvation from motion event flood (was hundreds of MotionNotify after menu ungrab).
5. **Diagnostic traces**: [REQ], [GetInputFocus], [GrabServer], [UngrabServer], [EVT_SEND], [OR_MOTION] traces added for debugging. [EVT_SEND] logs ALL event types sent to clients.

### Key xscope Finding
The clipboard fix was identified by comparing xscope wire captures of XQuartz (working) vs SwiftX11 (hanging). The critical differences:
- XQuartz sends FocusOut(mode=Ungrab) + FocusIn(mode=Ungrab) after UngrabKeyboard → we didn't
- XQuartz sends PropertyNotify after ChangeProperty → we didn't
- After both fixes, Java proceeds identically: InternAtom(CLIPBOARD) → InternAtom(clipboard MIME types) → ChangeProperty(_SUNW_JAVA_AWT_TIME) → SetSelectionOwner(CLIPBOARD)

## Known Issues (v1.15.22)

### Menu Item Highlighting (Vivado)
- **Symptom**: Menu items highlight very slowly (many seconds delay). Eventually highlights if user wiggles mouse or waits long enough.
- **Diagnosis so far**: Vivado uses lightweight popups (Java Swing default) — menus rendered within main window, not separate OR windows. [OR_MOTION] trace shows zero hits. ~8 MotionNotify events delivered during menu grab period.
- **Theory**: Not enough MotionNotify events reaching client during grab. Could be PointerMove coalescing too aggressive, or Cocoa mouseDragged routing (during button-held menus) not generating enough events.
- **Next step**: xscope comparison of motion events during XQuartz menu hover vs SwiftX11 to see event rate and coordinate differences.

### Other
- **Vivado startup banner**: Sometimes shows at 200×100 (when ConfigureWindow arrives after MapWindow). Deferred show mechanism mostly handles this.
- **xeyes shaped window black flash on resize**: Minor cosmetic issue during live resize.

## Debug Traces Still Active
- `[REQ]` — Every request opcode + seq (in readAndDispatch)
- `[GetInputFocus]` — GetInputFocus handler (opcode 43)
- `[GrabServer]` / `[UngrabServer]` — GrabServer/UngrabServer handlers
- `[EVT_SEND]` — ALL events sent via sendEvent32/sendEventVariable
- `[OR_MOTION]` — MotionNotify to override-redirect windows (first 10 + every 50th)
- `[SEL]` — SetSelectionOwner and ConvertSelection
- `[WM_HINTS_DBG]` — WM_NORMAL_HINTS parser
- `[MAP_SHOW]` — mapWindow geometry diagnostic
- `[CLIPBOARD]` — Clipboard bridge operations

These should be cleaned up once menu highlighting is resolved.

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
| `GrabOps.cpp` | FocusIn/FocusOut(mode=Ungrab) on UngrabKeyboard; [GrabServer]/[UngrabServer] traces |
| `PropOps.cpp` | PropertyNotify generation on ChangeProperty (with PropertyChangeMask filter) |
| `SelectionOps.cpp` | Proactive SelectionRequest for CLIPBOARD+PRIMARY on SetSelectionOwner |
| `EventOps.cpp` | [OR_MOTION] diagnostic trace for grab motion debugging |
| `QueryOps.cpp` | [GetInputFocus] trace |
| `XProtoDaemon.cpp` | [REQ] per-request opcode trace |
| `XProtoTransport.cpp` | [EVT_SEND] all-event-type trace |
| `HostCommandQueue.hpp` | PointerMove coalescing (consecutive same-window dedup) |
| `SwiftX11Version.h` | v1.15.22 |

## Priority for Next Session
1. **Fix Vivado menu highlighting** (xscope wire comparison needed)
2. **Clean up diagnostic traces** (remove [REQ], [EVT_SEND], [OR_MOTION], etc.)
3. **Update CLAUDE.md** with clipboard/PropertyNotify/FocusIn architecture
4. **Vitis testing** (Phase 8)
