# SwiftX11 — X11 Server for macOS

## Project Overview

SwiftX11 is an X11 protocol server running natively on macOS, implementing the X11 wire protocol so that X11 clients (xterm, xeyes, etc.) can display and interact via native Cocoa/Metal windows.

## Architecture

### Language Layers
- **Swift** (`macos/SwiftX11/`): UI owner — AppKit windows, Metal/software rendering, surface allocation, damage scheduling, networking (accepts X11 connections)
- **C++** (`macos/X11LowLevel/cpp/X11Protocol/`): Protocol core — request parsing, reply/event framing, raster drawing ops, resource tables (windows, pixmaps, GCs, fonts, atoms, colormaps), Swift bridge (`SwiftBridge.cpp` implements the `extern "C"` functions in `SwiftX11Bridge.h`)

### Key Design: Swift-Owned Surfaces
Swift allocates all WINDOW backing stores (host-level CPU buffers). C++ draws into these via `DrawableSurfaceRegistry`:

```
Swift: ensureHostSurface(wPx, hPx)
  → x11_surface_update(xid, ptr, bytesPerRow, w, h, generation)
    → DrawableSurfaceRegistry::set(hostXid, SurfaceDesc)
      → C++ drawing ops resolve via resolveDrawableRW()
```

**Critical**: `bytesPerRow` is 64-byte aligned, so `stridePixels = bytesPerRow/4` often differs from `width`. All pixel row indexing MUST use `dst.stridePixels`, never `dst.w`.

### Child Window → Host Surface Routing
Child windows (e.g., xterm's scrollbar, VT widget) do NOT have their own surfaces. They draw into the host (top-level) window's surface at an offset:

```
resolveDrawableRW(ctx, childXid, dst)
  → host = topLevelAncestorOf(childXid)
  → surface = DrawableSurfaceRegistry::get(host)
  → offset = computeOffsetInHost(host, childXid)  // walks parent chain
  → dst.pixels32 = surface.ptr + oy*stride + ox   // shifted pointer
  → dst.w = clipped child width, dst.h = clipped child height
  → dst.stridePixels = host surface stride
```

**Negative offsets**: X11 allows child windows at negative positions (e.g., xterm scrollbar at y=-1 to hide a border pixel). `resolveDrawableRW` clamps negative offsets to 0 and reduces the effective drawable dimensions by the clipped amount, shifting the child's drawing origin by at most a few pixels.

### Damage/Present Pipeline
```
C++ draw op → damageOrDirty(ctx, drawable, x, y, w, h)
  → routes to host window (topLevelAncestorOf)
  → translates child-local rect to host-surface coords
  → x11_shared_damage_union(host, x, y, w, h)  [shared accumulator, mutex-protected]
  → x11_ui_push_damage(host, x, y, w, h)       [UI command queue signal]
  → Swift: schedulePresent(host)
  → present timer fires (20ms coalesce)
  → x11_shared_damage_consume(host) → DamageRect
  → Metal: partial texture upload via MTLTexture.replace(region:)
  → Software: full CGImage blit (no partial support)
```

**Key design**: The shared damage accumulator (`x11_shared_damage_union/consume`) bypasses UI command queue drain latency. C++ writes at draw time, Swift reads at present time — zero queue delay. The UI command queue signal only triggers `schedulePresent()`; rect data comes from the accumulator.

### Resize Flow
```
Cocoa resize → X11WindowHost.handleDrawableSize()
  → ensureHostSurface(wPx, hPx)  [reallocates + registers surface]
  → x11_post_window_resize()
  → server thread: applyRootlessResize()
    → geometry update, ConfigureNotify + Expose to client
    → damageOrDirty()
```

### Window Lifecycle & Timing
```
CreateWindow → C++ WindowTable entry + pushCreate → Swift noteX11WindowCreated
  (host: creates NSWindow; child: metadata tracking only)
MapWindow → setMapped + fillWindowBackground + queue Expose
  (host: x11_ui_push_map + x11_ui_push_resize → Swift creates NSWindow)
  (NOTE: fillWindowBackground for children FAILS here — surface not registered yet)
Swift main thread → ensureHostSurface → x11_surface_update → x11_post_window_presentable
SetPresentable → setPresentable + fillWindowBackgroundIfReady + sendExposeSubtree
  (This is when backgrounds actually get painted and children get valid Expose)
```

### Surface Resize Re-expose
When `x11_surface_update` detects a surface size change (e.g., initial 64×64 → real 819×484), it queues a `SurfaceResized` host command via `x11_proto_bridge_surface_resized(hostXid)`. The handler calls `sendExposeSubtree` + marks dirty + pushes damage, ensuring all mapped children get re-exposed at the correct geometry. This fixes a timing race where `setContentSize` is deferred via `DispatchQueue.main.async` but surface registration happens at the NSWindow's default (small) size.

### Input Event Routing

**Button events** (XProtoServerBridge.cpp `HostCmdType::Button`):
1. Pick deepest mapped window under pointer BEFORE updating drag state
2. Check passive grabs (GrabButton) by walking from child up to host
3. Call `InputState::button(under, ...)` — sets `drag_xid` on 0→nonzero transition
4. Walk up from `under` to find window with ButtonPress/ButtonRelease mask
5. Deliver ButtonPress/ButtonRelease to that window

**Motion events** (XProtoNotifyBridge.cpp `postMotion`):
- During drag (`drag_xid != 0`): route directly to `drag_xid`
- Otherwise: `pick_motion_target` walks descendants, finds deepest mapped window containing pointer, then walks up to find PointerMotionMask

**Focus events** (XProtoServerBridge.cpp `HostCmdType::Focus`):
- Emulates WM SetInputFocus: sends FocusIn to HOST via `sendFocusEventDirect()` (bypasses FocusChangeMask check)
- Toolkit (Xt) receives FocusIn on shell → calls SetInputFocus (opcode 42) to propagate to child widget
- SetInputFocus handler sends FocusIn/FocusOut directly via `sendEvent32` (also bypasses mask check)

**Option+click → button 2**: Swift remaps Option+left-click to button 2 (middle mouse) for Xaw scrollbar thumb drag. Stored in `optionClickButton` for consistent mouseUp/drag handling.

**X11 state field**: `toX11State(buttons, mods)` maps internal button bits (0-4) to X11 wire positions (bits 8-12) and modifier bits to standard X11 positions. Used by all event builders (button, motion, crossing, key).

## Key Files

| File | Role |
|------|------|
| `SwiftX11/UI/Windows/X11WindowHost.swift` | Host window, surface allocation, Metal/software rendering, input events |
| `SwiftX11/Core/WindowRegistry.swift` | Cocoa window management, noteX11WindowCreated, mapWindow |
| `X11LowLevel/cpp/X11Protocol/include/Core/SurfaceDesc.hpp` | Surface descriptor struct |
| `X11LowLevel/cpp/X11Protocol/include/Core/DrawableSurfaceRegistry.hpp` | Thread-safe XID→surface registry |
| `X11LowLevel/cpp/X11Protocol/include/Core/DrawableRW.hpp` | Unified drawable abstraction (w, h, stridePixels, pixels32) |
| `X11LowLevel/cpp/X11Protocol/src/Core/DrawableRW.cpp` | resolveDrawableRW: Swift surface only, child→host offset routing |
| `X11LowLevel/cpp/X11Protocol/src/Ops/DrawOps.cpp` | PutImage, CopyArea, ClearArea, text ops |
| `X11LowLevel/cpp/X11Protocol/src/Ops/ShapeOps.cpp` | PolyArc, PolyFillArc, PolyFillRectangle, PolyLine, PolySegment, PolyPoint |
| `X11LowLevel/cpp/X11Protocol/src/Ops/WindowOps.cpp` | CreateWindow, MapWindow, ConfigureWindow, rootless resize |
| `X11LowLevel/cpp/X11Protocol/src/Ops/EventOps.cpp` | MotionNotify, ButtonPress/Release, FocusIn/Out, crossing events, XI2 event senders |
| `X11LowLevel/cpp/X11Protocol/src/Ops/GrabOps.cpp` | GrabPointer, GrabButton, GrabKeyboard, passive/active grabs |
| `X11LowLevel/cpp/X11Protocol/src/Ops/QueryOps.cpp` | SetInputFocus, GetInputFocus, QueryPointer, TranslateCoordinates |
| `X11LowLevel/cpp/X11Protocol/src/XProtoServerBridge.cpp` | Host command dispatch (Focus, Button, ScrollTicks, SetPresentable, SurfaceResized) |
| `X11LowLevel/cpp/X11Protocol/src/XProtoNotifyBridge.cpp` | Motion event routing, pick_motion_target, cursor application |
| `X11LowLevel/cpp/X11Protocol/src/SurfaceBridge.cpp` | Surface registration, size-change detection → SurfaceResized |
| `X11LowLevel/cpp/X11Protocol/src/Utils/Damage.hpp` | damageOrDirty() helper — translates child→host, writes shared accumulator + signals |
| `X11LowLevel/cpp/X11Protocol/src/UI/UICommandQueue.cpp` | UI command queue + shared damage accumulator (x11_shared_damage_union/consume/clear) |
| `X11LowLevel/cpp/X11Protocol/include/Core/XClient.hpp` | Per-connection state: transport, reply writer, client ID space |
| `X11LowLevel/cpp/X11Protocol/include/Core/HostCommandQueue.hpp` | Thread-safe host command queue (Cocoa → xproto thread) |
| `X11LowLevel/cpp/X11Protocol/include/Core/InputState.hpp` | Pointer/button/focus state, drag_xid management |
| `X11LowLevel/cpp/X11Protocol/include/Core/GrabTable.hpp` | Passive grab table (GrabButton) + active pointer grab (GrabPointer) |
| `X11LowLevel/cpp/X11Protocol/include/Core/X11Modifiers.hpp` | toX11State(): maps internal button/mod bits to X11 wire positions |
| `X11LowLevel/cpp/X11Protocol/include/Core/XEventMask.hpp` | X11 event mask constants (FocusChange, ButtonPress, PointerMotion, etc.) |
| `X11LowLevel/cpp/X11Protocol/src/SwiftBridge.cpp` | extern "C" bridge: lifecycle, input, resize, presentable (replaces x11_shim.c) |
| `X11LowLevel/cpp/X11Protocol/src/Transport/X11Setup.cpp` | X11 connection setup handshake (replaces x11_xproto.c) |
| `SwiftX11/Core/XServerController.swift` | Server startup, version banner |
| `SwiftX11/UI/Windows/X11MetalRenderer.swift` | Metal texture management, partial sub-rect uploads |
| `X11LowLevel/include/SwiftX11Version.h` | Single source of truth for version string |
| `X11LowLevel/cpp/X11Protocol/include/Utils/FillStyle.hpp` | GC fill-style helper (Solid/Tiled/Stippled/OpaqueStippled) |
| `X11LowLevel/cpp/X11Protocol/include/Core/ShapeRegion.hpp` | SHAPE extension region storage (bounding/clip/input shape rects) |
| `X11LowLevel/cpp/X11Protocol/include/Core/PropertyTable.hpp` | Shared property storage (used by PropOps + SelectionOps) |
| `X11LowLevel/cpp/X11Protocol/include/Core/ClipboardAtoms.hpp` | Well-known atom constants (CLIPBOARD, TARGETS, UTF8_STRING, etc.) |
| `X11LowLevel/cpp/X11Protocol/src/Ops/SelectionOps.cpp` | Selection protocol + macOS clipboard bridge |
| `X11LowLevel/cpp/X11Protocol/include/Core/ScreenLayout.hpp` | Multi-monitor layout cache (MonitorInfo, ScreenLayout) |
| `X11LowLevel/cpp/X11Protocol/src/Core/ScreenLayout.cpp` | CGDisplay query, CGDisplayReconfigurationCallback |
| `X11LowLevel/cpp/X11Protocol/include/Core/XI2EventMask.hpp` | XI2 event types, masks, wire sizes (DeviceEvent/Enter/RawEvent) |
| `X11LowLevel/cpp/X11Protocol/include/Core/X11ExtOpcodes.hpp` | Extension opcodes, first_event/first_error values |
| `SwiftX11/Core/GlobalPointerTracker.swift` | Global mouse monitor (NSEvent global+local), drives XI2 RawMotion |
| `docs/TODO.md` | Comprehensive project roadmap |

## Build & Run

- Xcode project: `macos/SwiftX11.xcodeproj`
- Build target: SwiftX11 (macOS app)
- Test clients: `xeyes`, `xterm` connected via `DISPLAY=127.0.0.1:1` (SwiftX11 runs on display :1 to avoid XQuartz conflict on :0)
- Default DISPLAY is set in `~/.profile`: `export DISPLAY=127.0.0.1:1`
- Scrollbar test: `xterm -sb -rightbar -bc`
- Scrollbar thumb drag: Option+click+drag (emulates middle button)

## Development Guidelines

### Drawing Operations
- Always resolve drawables via `resolveDrawableRW(ctx, drawable, dst)` before accessing pixels
- Use `dst.stridePixels` (not `dst.w`) for row-to-row stepping in all pixel indexing
- Force alpha opaque (`| 0xFF000000u`) for XRGB8888 surfaces
- Call `damageOrDirty(ctx, drawable)` after modifying window pixels
- PolyFillRectangle is the reference pattern for correct stride-aware rasterization

### Event State Fields
- **Always** use `x11::input::toX11State(buttons, mods)` for the state field in all events (button, motion, crossing, key)
- Internal button bits are at positions 0-4; X11 wire format requires bits 8-12
- Internal modifier bits: Shift=0, Ctrl=1, Alt/Option=2, Cmd=3; X11 wire: Shift=0, Ctrl=2, Mod1=3, Mod4=6

### Input Event Patterns
- Button handler picks deepest window BEFORE `InputState::button()` (which sets `drag_xid`)
- Passive grabs (GrabButton) are checked by walking from deepest child up to host
- Motion events route to `drag_xid` during active drags, bypassing `pick_motion_target`
- Focus uses `sendFocusEventDirect()` (bypasses FocusChangeMask) to emulate WM SetInputFocus behaviour
- **Do NOT add click-to-focus in Button handler** — Focus handler (Cocoa becomeKey) handles it

### XI2 (XInput2) Event Delivery
- XI2 events are GenericEvent (type 35), variable-length, sent via `sendEventVariable()`
- **RawMotion** is a ROOT-level event: sent in `postMotion()` BEFORE `if (!deliver) return`, so `GlobalPointerTracker`'s global mouse monitor delivers it even when cursor is outside X11 windows
- RawMotion events must include valuator data (68 bytes with 2 axes, length=9) — `XInputWireToCookie` in libXi fails on length=0
- Per-window XI2 masks (stored in `WindowView.xi2_mask`) OR'd with `InputState.xi2_root_mask` for effective mask check
- **Extension advertisement**: XInputExtension must return `first_event >= 64` in QueryExtension — libXi's `XextAddDisplay` calls `XESetWireToEvent(dpy, first_event+i, ...)` for 17 event types; `first_event=0` overwrites core event handlers (Expose, MotionNotify, etc.)
- XI2 events use `XESetWireToEventCookie` (keyed by major opcode, not first_event) for GenericEvent dispatch

### Reply Requirements
- Requests that generate replies MUST call `ctx.reply().sendReply32(seq, ...)` — missing replies cause XCB sequence desync crashes
- GrabPointer (opcode 26) and GrabKeyboard (opcode 31) replies both use `sendReply32(seq, ...)`

### Surface Lifecycle
- Surfaces are keyed by **host (top-level) window XID** in the registry
- Child windows resolve to host surface + offset via `computeOffsetInHost`
- `generation` counter tracks reallocations; C++ can detect stale pointers
- On window destruction, `x11_surface_clear(xid)` removes the registry entry

### Debugging
- **Two trace tiers**:
  - `#ifndef NDEBUG` (debug builds): Key lifecycle/diagnostic traces that are always on in debug — `[LIFECYCLE]`, `[BG_FILL]`, `[BG_FILL_RETRY]`, `[EXPOSE_SUBTREE]`, `[SET_PRESENTABLE]`, `[SURFACE_UPDATE]`, `[SURFACE_RESIZED]`, `[BTN_GRAB]`, `[BTN]`, `[FOCUS_DIRECT]`, `[SetInputFocus]`, `[GrabPointer]`, `[UngrabPointer]`, `[DRAG_MOTION]`
  - `#ifdef X11_TRACE_VERBOSE`: High-frequency per-op traces gated behind a separate flag — `[RESOLVE]`, `[COPY_SURFACE]`, `[DAMAGE]`, `[DISPATCH]`, `[FOCUS]`, `[SCROLL]`, `[KEY]`, `[TEXT]`, `[CopyArea]`, `[ClearArea]`, `[CROSS]`, `[SEND]`, plus all `ctx.tracef()` calls (~71 call sites silenced at once)
- To enable verbose tracing: add `-DX11_TRACE_VERBOSE` to OTHER_CPLUSPLUSFLAGS in Xcode build settings
- Watch for stride vs width mismatches — the most common class of rendering bug
- **Version banner**: `SwiftX11 v{version}` printed at startup (Swift `XServerController.buildVersion` + C++ `kSwiftX11Version`). Bump version when making changes to verify the correct build is running.

### Version Numbering Convention
- **Release**: `SWIFTX11_VERSION_BASE` (e.g., "1.19.35")
- **Debug iterations**: `SWIFTX11_DEBUG_BUILD` counter appends `.N-dbg` (e.g., "1.19.35.12-dbg"). Set to 0 for release.
- Both defined in `X11LowLevel/include/SwiftX11Version.h`

### Development Workflow
- **Worktree**: Claude Code uses a git worktree under `.claude/worktrees/`. Changes are committed there, then merged to `develop` for testing.
- **Standard practice**: Commit on worktree branch, then `cd /Users/lkg/Documents/Vivado/SwiftX11/macos && git merge <branch> --no-edit` to fast-forward `develop`.
- **Building**: User builds from Xcode (Cmd+B). Avoid `xcodebuild` from CLI as it can interfere with Xcode.app.
- **Docker image rebuilds**: The `x64-linux-dbus` Docker image bakes in `docker-entrypoint.sh`. After editing the entrypoint, rebuild: `cd /Users/lkg/Documents/Vivado/vivado2023 && docker build --platform linux/amd64 -t x64-linux-dbus -f Dockerfile .`

### Vitis (Electron) Support — Known Issues (v1.19.35)
- **XInputExtension must be hidden** from Chromium/Electron: `present=0` in QueryExtension. Without this, Electron's XI2 initialization (XIQueryDevice) causes immediate disconnect. Workaround in place; root cause under investigation (see TODO.md 8.11).
- **`_MOTIF_WM_HINTS decor=0`**: Electron sets this for custom chrome. Our handler makes the window borderless + `isMovableByWindowBackground=true` at `.normal` level (not `.floating`).
- **`--disable-gpu`** flag required for Electron in Docker/Rosetta. Injected via entrypoint patching of the Vitis wrapper script (`sed` on line 567 of `/Xilinx/2025.1.1/Vitis/bin/vitis`).
- **Vitis works perfectly under xpra** with full XI2 — this confirms the issue is specific to our XI2 implementation, not the Docker/Rosetta environment.

### Current State (v1.19.35)
- **C layer eliminated** (v1.0.0): All C source files (x11_shim.c, x11_backend.c, x11_requests.c, x11_xproto.c) and their headers removed (~2,600 lines). Architecture is now Swift ↔ C++ (extern "C" via SwiftBridge.cpp) — no intermediate C layer
- **No C request queue**: UICommandQueue::push() calls x11_ui_push_*() directly. No C runloop thread. HostCommandQueue handles all Cocoa→server communication
- `resolveDrawableRW` is Swift-surface-only (no C FB fallback)
- Host windows resolve directly to their Swift surface
- Child windows resolve to host surface + computed offset (with negative offset clamping)
- Present path copies the single host surface (no compositing)
- `fillWindowBackgroundIfReady` in `sendExposeSubtree` retries background fills after SetPresentable
- `SurfaceResized` mechanism re-exposes children when surface dimensions change
- **Damage rects threaded end-to-end**: C++ draw ops report precise rects → shared accumulator → Metal partial texture upload
- **Shared damage accumulator**: `x11_shared_damage_union/consume` in UICommandQueue.cpp — mutex-protected, 64-entry fixed array, bypasses UI command queue drain latency
- **Metal partial uploads**: `X11MetalRenderer.updateTexture()` uses `MTLTexture.replace(region:)` for sub-rect uploads; `fullUploadCountdown` forces full uploads for first 3 frames after texture creation
- **Focus delivery**: `sendFocusEventDirect()` bypasses FocusChangeMask check, emulating WM SetInputFocus. Sends FocusIn to HOST on Cocoa becomeKey; toolkit (Xt) propagates to children via SetInputFocus (opcode 42).
- **Focus guard** (v1.0.3): Stale FocusOut from destroyed non-focused windows no longer steals focus from the actual focus holder. FocusOut path guarded by `focus_host == host` check.
- **GC clipping** (v1.1.0): SetClipRectangles (opcode 59) fully implemented. GCState stores clip_rects vector, clip_x_origin/y_origin, has_clip flag. CreateGC/ChangeGC handle clip valuemask bits 17-19, CopyGC copies clip state. All draw ops enforce clip: PolyFillRectangle uses rect-intersection via `gcClipForEachRect`, per-pixel ops (lines, arcs, text, CopyArea) use `gcPointVisible`. PutImage uses rect-level clipping for efficiency. `Utils/GCClip.hpp` provides the clip utility functions.
- **Retained display buffer** (v1.1.1): Pre-resize frame kept for flicker-free resize.
- **GC value mask completion** (v1.2.0): All 23 GC value mask bits (0-22) now parsed and stored in GCState. Added: line_width, line_style, cap_style, join_style, fill_style, fill_rule, tile, stipple, ts_x/y_origin, dash_offset, dashes_single, arc_mode, dash_list. CopyGC copies all fields.
- **SetDashes** (v1.2.0, opcode 58): Stores dash offset and dash list in GCState.
- **ReparentWindow** (v1.2.0, opcode 7): Updates parent chain in WindowTable, sends ReparentNotify event, handles unmap/remap if window was mapped.
- **DestroySubwindows** (v1.2.0, opcode 5): Destroys all descendants in depth-first order via descendantsOf().
- **Reply-bearing stubs** (v1.2.0): QueryKeymap (44, all-zeros), GetMotionEvents (39, empty list), GetFontPath (52, empty list) — prevent XCB sequence desync.
- **Void stubs** (v1.2.0): SetFontPath (51), ChangeActivePointerGrab (30, updates active grab event mask), RotateProperties (114).
- **Server-drawn window borders** (v1.3.0): `border_width` and `border_pixel` stored in WindowState/WindowView. `fillWindowBorder()` renders border strips in parent's drawable. All 5 parent-chain offset computations account for border_width (drawable starts at x+bw, y+bw in parent coords). Hit testing in pick functions includes border region. ChangeWindowAttributes handles CWBorderPixel, ConfigureWindow handles CWBorderWidth, GetGeometry returns border_width.
- **Button routing fix** (v1.3.0): `updateMotion()` no longer overwrites `buttons` field (was corrupted by PointerMove with deliver=0 between press and release). Drag logic resilient to stale `before` values. Button handler uses HostCmd coordinates instead of stale InputState position. Picks deepest mapped child before `InputState::button()`, checks passive grabs (GrabButton), correctly sets `drag_xid` to child window.
- **GC function/planemask** (v1.4.0): All shape draw ops (PolyFillArc, PolyArc, FillPoly, PolyLine, PolySegment, PolyPoint) now apply GC ROP function and planemask via `applyGCFunction()`.
- **WarpPointer** (v1.4.0, opcode 41): Routes via UICommandQueue → Swift → CGWarpMouseCursorPosition. Translates X11 coordinates to screen coordinates.
- **BIG-REQUESTS** (v1.4.0): Per-client `big_req_enabled_` flag. Extended-length parsing in readAndDispatch (phase 0.5: len_words==0 → read 4 extra bytes as 32-bit length). BigReqEnable (opcode 133) replies with max_request_length=1M words (4MB).
- **16-bit text** (v1.4.0): PolyText16 (opcode 75) and ImageText16 (opcode 77) with CHAR2B encoding (byte1 high, byte2 low).
- **Extension stubs** (v1.4.0→v1.7.5): Handler code for XFIXES (134, v5.0), SHAPE (135, v1.1), RANDR (136, v1.3), Xinerama (137, v1.1 + IsActive + QueryScreens), GE (138, v1.0). Opcodes defined in X11ExtOpcodes.hpp. **All now advertised** (v1.7.4–v1.7.5). RANDR dynamically reports real multi-monitor configuration via ScreenLayout (v1.9.7). SHAPE fully implemented with actual clipping (v1.7.5).
- **RENDER extension** (v1.4.0, major 139): Handler code for QueryPictFormats (ARGB32/RGB24/A8/A4/A1), CreatePicture/FreePicture, Composite (PictOpSrc/Over/Add/Clear), FillRectangles, CreateSolidFill, QueryFilters. Glyph ops are stubs (consume silently). Picture table tracks drawable/format/solid state. **NOT advertised to clients** — needs Trapezoids + CompositeGlyphs rendering before enabling.
- **Selections/clipboard bridge** (v1.5.0): Pre-registered atoms 69-77 (CLIPBOARD, TARGETS, UTF8_STRING, TIMESTAMP, TEXT, MULTIPLE, INCR, WM_PROTOCOLS, WM_DELETE_WINDOW). PropertyTable extracted to shared header. Swift registers get/set callbacks via `x11_clipboard_register()`. ConvertSelection reads NSPasteboard when no X11 owner. SendEvent intercepts SelectionNotify for CLIPBOARD → NSPasteboard write. TARGETS returns supported types.
- **GC fill-style rendering** (v1.5.0): FillSolid/FillTiled/FillStippled/FillOpaqueStippled implemented in PolyFillRectangle, FillPoly, PolyFillArc via `Utils/FillStyle.hpp` helper. Tile pixmaps sampled at `(px-ts_x_origin)%tile_w`. Stipple bitmaps bit-tested with proper modulo wrapping. Solid fill fast path preserved (zero overhead when fill_style==0).
- **Bidirectional clipboard sync** (v1.5.1): X11→macOS direction completed. ClipboardCapture in PropOps detects UTF8_STRING/STRING property writes during selection transfer, pushes text to NSPasteboard via Swift callback. Root-proxy selection owner (XID 1) assigned after capture so subsequent X11 ConvertSelection routes through server (serves from cached macOS clipboard). Sequence number stamping fix in handleSendEvent ensures correct SelectionNotify delivery. macOS→X11: ConvertSelection from xterm triggers server to read NSPasteboard and serve directly.
- **CWBackPixmap/ParentRelative** (v1.5.2): CreateWindow and ChangeWindowAttributes now handle CWBackPixmap (bit 0). Value 1 (ParentRelative) walks parent chain via `resolveParentRelativeBackground()` to copy nearest ancestor's background_pixel. Value 0 (None) calls `clearBackground()` to disable server background fill. Both methods added to WindowTable.
- **Child-to-child crossing events** (v1.5.5): postMotion now generates EnterNotify/LeaveNotify when the pointer moves between child windows within the same host. Previously, crossing events only fired at NSWindow boundaries, causing Xaw Command widget hover highlighting to get stuck (e.g., xcalc ON button stayed highlighted).
- **ConfigureWindow CWStackMode** (v1.5.6): ConfigureWindow now parses CWSibling (bit 5) and CWStackMode (bit 6). WindowTable has raiseToTop/lowerToBottom/restackAbove/restackBelow methods. Supports Above/Below/TopIf/BottomIf/Opposite stacking operations.
- **Occluded rectangle support** (v1.5.6): resolveDrawableRW now computes occluded rectangles for partial sibling overlaps that edge-based clipping can't handle. ClearArea, fillWindowBackground, fillWindowBackgroundIfReady, and PolyFillRectangle skip writing to pixels covered by higher-stacking mapped siblings. This prevents lower-stacking windows (e.g., xcalc's display widget) from erasing content of higher-stacking siblings (buttons 40-54) at partial overlap zones.
- **PutImage resolveDrawableRW** (v1.5.7): PutImage's WINDOW path now uses resolveDrawableRW instead of manual parent-chain walking. Fixes: (1) missing border_width in offset computation caused PutImage to be misplaced by border_width pixels, (2) no child-bounds clipping allowed PutImage to write beyond child window into sibling areas, (3) fast GXcopy path now forces alpha opaque (was raw memcpy without alpha forcing).
- **MapSubwindows/UnmapSubwindows direct-children fix** (v1.5.8): Both handlers now use `childrenInStackOrder()` (direct children only) instead of `descendantsOf()` (all descendants). X11 spec: "MapSubwindows is equivalent to performing a MapWindow request on each unmapped child." The old code mapped ALL descendants, which broke Xt's `mappedWhenManaged: False` — buttons that Xt intentionally didn't map got mapped by MapSubwindows on the shell. Fix confirmed via xcalc -rpn buttons 21-22 (hidden on XQuartz, previously visible on SwiftX11).
- **XFILESEARCHPATH auto-setup** (v1.5.8): SwiftX11 calls `setenv("XFILESEARCHPATH", ...)` and `launchctl setenv` at startup so X11 clients find app-defaults at `/opt/X11/share/X11/app-defaults/` without manual user configuration. The compiled-in libXt default (`/usr/lib/X11/...`) doesn't exist on macOS.
- **Motion state**: `toX11State()` used everywhere (button bits at X11 positions 8-12)
- **Key event modifiers** (v1.0.1): `sendKeyEvent()` uses `toX11State()` for correct modifier mapping (was previously using raw `buttons | mods` which mapped Option→Control, Control→Shift)
- **GrabPointer reply** (v1.0.2): Re-enabled — uses same `sendReply32()` pattern as GrabKeyboard. Missing reply was causing XCB sequence desync on scrollbar use.
- **Window focus on map** (v1.0.2): `mapWindow()` uses `makeKeyAndOrderFront()` instead of `orderFront()` so new windows receive focus and cursor blink starts immediately.
- **Option+click → button 2**: macOS middle-mouse emulation for Xaw scrollbar thumb drag
- xterm with scrollbar (`xterm -sb -rightbar -bc`) works correctly — cursor blinks, scrollbar stays visible, trackpad scrolling works, Option+click thumb drag works
- xeyes works correctly; killing xeyes while xterm has focus no longer breaks xterm cursor blink
- **Multi-client architecture**: Server-wide state split from per-client state
  - `XProtoServer` is persistent — survives across client sessions, owns: `WindowTable`, `PixmapTable`, `FontTable`, `CursorTable`, `DrawableSurfaceRegistry`, `GrabTable`, `InputState`, `UICommandQueue`, `HostCommandQueue`
  - `XClient` holds per-connection state: `XProtoTransport`, `ReplyWriter`, fd, rid_base/rid_mask
  - `XProtoDaemon` owns the server (lazy init on first session) and creates `XClient` per connection
  - `XProtoContext` has `setClient()/clearClient()` to wire per-client transport into the shared context
  - Remaining globals: `g_daemon` (process-lifetime), `g_daemon_ptr` (bridge access), `g_ctx/g_ev/g_q` (NotifyBridge, set per-session)
- **Display :1**: SwiftX11 runs on display :1 (TCP port 6001) to avoid conflict with XQuartz on :0. `~/.profile` sets `DISPLAY=127.0.0.1:1`.

- **Phase 3 font infrastructure** (v1.7.0): XLFD wildcard matching, PCF font support (.pcf.gz from system directories), font aliases, ListFontsWithInfo, Symbol font encoding. PCF parser handles MSB/LSB bit order with byte-level bit reversal. Lazy-loads fonts on demand from `/opt/X11/share/fonts/{misc,75dpi,100dpi}/`.
- **xcalc Symbol characters fixed** (v1.7.0): sqrt (√), division (÷), pi (π) now render correctly from Symbol PCF font (`symb12.pcf.gz`, `adobe-fontspecific` encoding).
- **App sandbox disabled** (v1.7.0): `ENABLE_APP_SANDBOX=NO` in Xcode build settings. The sandbox blocked `std::ifstream` from opening `/opt/X11/share/fonts/*/fonts.dir` (errno=1 EPERM), causing 0 PCF fonts to be registered. Root cause of Symbol font failure — all OpenFont requests fell through to the builtin "fixed" Latin-1 font.
- **Font alias PCF resolution** (v1.7.0): `loadAliases()` now resolves alias targets against PCF registry (exact + glob match), not just builtins. Increased resolved aliases from 0 to 99.
- **Categorical trace system** (v1.7.0+): `TraceDefs.hpp` provides opt-in trace categories (RESIZE, PRESENT, LIFECYCLE, INPUT, RESOLVE, FONT, RENDER) gated by `-DX11_TRACE_<CATEGORY>` flags. `X11_TRACE_VERBOSE` enables all. Font and RENDER traces use `X11_TRACE_FONT_ENABLED` / `X11_TRACE_RENDER_ENABLED`.

- **RENDER Triangles + Gradients** (v1.7.1): Triangles/TriStrip/TriFan (minor 11/12/13) with scanline rasterization. Gradient source pictures (CreateLinearGradient/Radial/Conical) with per-pixel sampling. All RENDER Phase 2.1 items complete.
- **XCB sequence desync safety net** (v1.7.2): Reply-sent tracking (`reply_sent_` flag), core opcode safety net (`isReplyBearingCore()` 128-entry table sends `BadImplementation` if reply-bearing opcode handler doesn't send a reply), extension error replies (all `default` cases send `BadRequest` instead of silently consuming bytes).
- **Monotonic wire-sequence floor** (v1.7.3): `XProtoTransport::sendAll()` enforces that response sequences never go backwards on the wire. Tracks `max_wire_seq_` and bumps stale sequences forward. Payload-aware: `payload_remaining_` counter distinguishes reply payload chunks from response headers. Fixes xcalc resize crash caused by `drainHostCommands()` interleaving with `readAndDispatch()`.
- **ExposeChildren border/background fix** (v1.7.3): `ExposeChildren` handler now calls `fillWindowBorderIfReady()` and `fillWindowBackgroundIfReady()` for each child, matching `sendExposeSubtree` behavior. Fixes button borders disappearing after window resize.

- **RANDR/Xinerama/GE advertised** (v1.7.4, dynamic v1.9.7): RANDR (RRQueryVersion 1.3, GetScreenResources/Current, GetOutputInfo, GetCrtcInfo, GetCrtcTransform, GetPanning, SetPanning, GetOutputPrimary, GetProviders, SelectInput, SetCrtcConfig, GetScreenSizeRange, ListOutputProperties, GetCrtcGammaSize), Xinerama (QueryVersion, IsActive, QueryScreens), and GE (QueryVersion) now return present=1 in QueryExtension and are listed in ListExtensions. RANDR and Xinerama dynamically report real multi-monitor layout via ScreenLayout cache (v1.9.7). Total advertised: 6 (BIG-REQUESTS, RENDER, XFIXES, RANDR, XINERAMA, GE).

- **SHAPE extension with actual clipping** (v1.7.5): Full SHAPE implementation — non-rectangular windows with real visual clipping and transparent backgrounds. ShapeRegion data structure stores bounding/clip/input regions per window. Wire protocol parsing for ShapeRectangles (minor 1), ShapeMask (minor 2), ShapeCombine (minor 3), ShapeOffset (minor 4). ShapeQueryExtents (minor 5) and ShapeGetRectangles (minor 8) return real data. Depth-1 pixmap drawing added to PolyFillArc and PolyFillRectangle (xeyes creates elliptical masks via XFillArc on depth-1 bitmaps). Hit testing respects shape regions (InputRouting + XProtoNotifyBridge). Present-time alpha masking in Swift: premultiplied alpha (transparent pixels = 0x00000000), shape rects queried via bridge. NSWindow + CAMetalLayer + SwiftUI layer hierarchy all set non-opaque for shaped windows. Metal pipeline uses alpha blending (sourceAlpha/oneMinusSourceAlpha). Retained display buffer skipped for shaped windows during resize. Total advertised extensions: 7 (BIG-REQUESTS, RENDER, XFIXES, RANDR, XINERAMA, GE, SHAPE).

- **CoreText font bridge** (v1.8.0): Maps X11 font requests (XLFD or bare names) to macOS system fonts via CoreText C API. Family mapping: fixed→Menlo, courier→Courier, helvetica→Helvetica, times→Times New Roman, lucida→Lucida Grande. XLFD parsing extracts family, weight (bold), slant (italic), pixel size. Rasterizes Latin-1 glyphs (0-255) with both 1-bit bitmap and 8-bit alpha coverage via `CGBitmapContext` + `CTFontDrawGlyphs`. New `drawGlyphAlpha32` lambda in all 4 text handlers (PolyText8/16, ImageText8/16) performs per-pixel alpha blending. Runtime toggle: `std::atomic<bool> g_antialiased` controlled via Settings → Rendering → "Antialiased Fonts". CoreText fonts tried first in FontTable lookup chain (after builtins/aliases/exact PCF), PCF/BDF fallback for cursor/symbol/fontspecific encodings. New files: `CoreTextFont.hpp/cpp`. Bridge: `x11_set/get_font_antialiased()`.

- **Glyph positioning fix** (v1.8.2): Corrected `topY = y - bbx_yoff - (bbx_h - 1)` to `topY = y - bbx_yoff - bbx_h` (matching X.org reference) at all 4 text handlers. Old formula placed glyphs 1px too low, causing descender bottom pixels to extend below ImageText8 background fill and get overwritten by the next line. Also fixed CompositeGlyphs source picture resolution for 1×1 Repeat pixmaps (old Xft text color pattern).

- **ARGB32 glyph format** (v1.8.3): AddGlyphs now handles ARGB32 (subpixel/LCD) glyph format. Previously `alphaBpp()` returned 8 for ARGB32, causing the A8 parser to read 1/4 of each glyph's data and corrupt all subsequent glyphs. New 32bpp path reads full CARD32 pixels, extracts alpha channel. RENDER trace category added to `TraceDefs.hpp`.

- **Window close → client kill** (v1.9.0): Red close button and Cmd+W now terminate X11 clients. ICCCM-compliant: checks WM_PROTOCOLS property for WM_DELETE_WINDOW atom, sends ClientMessage if supported. Falls back to forceful socket disconnect for clients without WM_DELETE_WINDOW. New `HostCmdType::WindowClose` host command, `x11_post_window_close()` bridge function. Pre-registered atoms: kWM_PROTOCOLS=76, kWM_DELETE_WINDOW=77.

- **X11 error handling** (v1.9.3): Proper X11 error generation across ~50 request handlers in 14 files. Previously, ~166 resource lookup sites silently skipped operations on missing resources; only 2 explicit error sends existed. Now sends spec-compliant errors: BadWindow (windows), BadDrawable (drawables), BadPixmap (pixmaps), BadFont (fonts), BadAtom (atoms), BadColor (colormaps), BadValue (invalid parameters). Three tiers: reply-bearing requests (critical for XCB desync prevention), void resource-modifying requests (spec compliance), and drawing ops. Uses existing infrastructure: `ctx.transport().sendErrorCore(error, seq, resourceId, major)`. Root XID (kRootXid) and None (0) bypass validation where appropriate.

- **Container/network support** (v1.9.4): Multi-listener architecture for Docker workflow. XProtoDaemon now supports both TCP and Unix domain socket listeners simultaneously. `listen_fd_` replaced with `vector<int> listen_fds_`. New `make_listen_socket_unix()` creates `/tmp/.X11-unix/X{display}` with mode 0777 for container access. TCP default bind changed from `127.0.0.1` to `0.0.0.0` so Docker containers using `host.docker.internal:1` can connect. New bridge function `x11_start_server_ex(display, enableTCP, enableUnix, bindAddr)`. Settings UI wired: NetworkTab has TCP/Unix toggles with Docker usage instructions. Poll loop updated for multiple listener sockets. Unix socket auto-cleaned on server stop. **Note**: Docker Desktop for Mac runs containers in a Linux VM — Unix socket volume mounts do not work (VirtioFS doesn't support socket files). Use TCP (`DISPLAY=host.docker.internal:1`) for Docker on macOS. Unix socket works for native macOS X11 clients (`DISPLAY=:1`).

- **Window management attributes** (v1.9.5): Full parsing, storage, and reporting of override_redirect (bit 9), win_gravity (bit 5), bit_gravity (bit 4), and backing_store (bit 6) in CreateWindow/ChangeWindowAttributes. GetWindowAttributes returns actual stored values instead of hardcoded zeros. ConfigureNotify/ReparentNotify use actual override_redirect and border_width. Override-redirect windows create borderless floating NSWindows (`.borderless` style mask + `.floating` level) and don't steal keyboard focus on map. WindowTable has setter methods: `setOverrideRedirect`, `setWinGravity`, `setBitGravity`, `setBackingStore`. UI command pipeline carries `X11_UI_FLAG_OVERRIDE_REDIRECT` flag from C++ through `x11_ui_cmd_t.flags` → Swift `noteX11WindowCreated(overrideRedirect:)` → `X11WindowController`. Gravity values stored but actual gravity-based resize adjustment deferred (toolkits handle client-side). Backing store stored as NotUseful stub.

- **UX bug fixes** (v1.9.6): Three fixes: (1) **Ctrl+click → button 3** — macOS Ctrl+click now correctly maps to X11 right-click (button 3), matching macOS convention. Previously Ctrl+click sent button 1 with Ctrl modifier. (2) **Window persistence fix** — `windowDidChangeOcclusionState` no longer unmaps windows when other macOS apps obscure them. Previously, switching to another app permanently hid X11 windows via `orderOut(nil)`. (3) **QueryTree root children** — `queryTree()` now handles root window (XID 1) which isn't in WindowTable's `map_` but has children in `children_order_`. `xwininfo -root -tree` and `xprop` now correctly show top-level X11 windows.

- **Multi-monitor support** (v1.9.7): ScreenLayout cache queries `CGGetActiveDisplayList` for real per-monitor data (position, size in points, pixel dimensions, physical mm). Auto-refreshes via `CGDisplayRegisterReconfigurationCallback`. Dynamic RANDR: `RRGetScreenResources/Current` reports N outputs/CRTCs/modes from real displays. `RRGetOutputInfo` uses 36-byte reply (not 32). New handlers: `RRGetCrtcTransform` (96-byte identity reply), `RRGetPanning/SetPanning`, `RRGetProviders`. Fixed 4 wrong RANDR minor opcode numbers. Dynamic Xinerama: `QueryScreens` returns N real screens. X11↔macOS coordinate conversion uses virtual desktop union bounds (`NSScreen.screens` min/max) for all 4 conversion sites. `GetGeometry` on root returns actual virtual desktop dimensions. `WarpPointer` uses virtual desktop bounds. OR window Metal drawable retry (up to 5×50ms) when `drawableSize == 0` after cross-screen `setFrame`. `xrandr --query` works. New files: `ScreenLayout.hpp/cpp`.

- **Metal-only rendering + performance** (v1.10.0): Software (CGImage/CALayer) rendering path removed entirely — Metal is now required. Removed `setupSoftwareLayer()`, `presentSoftware()`, `presentSoftwarePartial()`, `makeCGImage()`, `imageLayer`, `usingMetal`/`wantsMetal` flags, and "Use Metal Rendering" settings toggle. `setUseMetal()` replaced with `ensureMetalSetup()` (lazy one-time init). **PutImage bulk memcpy** — GXcopy fast path uses `std::memcpy` for row data then 4-pixel-unrolled alpha forcing, instead of per-pixel copy-and-OR. **Expose two-pass** — `sendExposeSubtree` and `ExposeChildren` restructured to fill all backgrounds/borders first, then send Expose events.

- **Multi-monitor popup fix** (v1.10.7): Three fixes for override-redirect (popup) windows on multi-monitor setups: (1) **Blank popup text on external monitors** — `CAMetalLayer.contentsScale` stayed 0.0 for `.borderless` NSWindows on external monitors (AppKit doesn't auto-inherit backingScaleFactor). Fixed by explicit `contentsScale = window.backingScaleFactor` in `ensureMetalSetup()` + `viewDidChangeBackingProperties`. (2) **Hot-plug popup positioning** — xterm doesn't query RANDR so Xlib's `WidthOfScreen`/`HeightOfScreen` remain stale after monitor changes, causing popups on wrong screen. Server-side `adjustOROriginForCursorScreen()` detects popup landing on different screen than cursor and repositions. `x11_set_window_position()` syncs X11 WindowTable geometry so input coordinates stay correct. (3) **ScreenLayoutChanged host command** — `CGDisplayReconfigurationCallback` triggers broadcast of ConfigureNotify + RRScreenChangeNotify to all connected clients on monitor hot-plug/unplug.

- **Comprehensive keyboard support** (v1.11.0): Full macOS virtual keycode → X11 keysym mapping covering all keys: letters (US), digits, punctuation, F1-F20, Home/End/PageUp/PageDown/Delete/Help, arrows, keypad (KP_0-9, operators, Enter, Equal), modifiers (left+right), CapsLock, Fn→Meta_L, ISO Section key. GetKeyboardMapping returns 4 keysyms per keycode (normal/shift/mode_switch/mode_switch+shift) for Java Swing/GTK compatibility. GetModifierMapping returns 2 keys per modifier (left+right: Shift_L/R, Control_L/R, Alt_L/R→Mod1, Super_L/R→Mod4). QueryKeymap returns real pressed-key state via InputState::keymap_ bitfield. ~90 keysym constants added to KeySyms.hpp.

- **Window menu + WM_NAME title sync** (v1.11.1): macOS Window menu via `NSApp.windowsMenu` in AppDelegate — AppKit auto-lists all X11 NSWindows and brings selected window to front. WM_NAME (atom 39) and `_NET_WM_NAME` (atom 79) property changes in PropOps trigger `x11_ui_push_title()` → NSWindow title update. Child window titles route through `topLevelAncestorOf()`. `_NET_WM_NAME` pre-registered as atom 79 in AtomTable.

- **ICCCM/WM compliance** (v1.12.0): Full ICCCM WM_NORMAL_HINTS parser (PSize/PMinSize/PMaxSize/PResizeInc/PBaseSize → NSWindow contentMinSize/contentMaxSize/contentResizeIncrements). WM_HINTS (InputHint → wants_input, StateHint → miniaturize on map). WM_TAKE_FOCUS protocol (ClientMessage before FocusIn, `wants_take_focus` cached in WindowView). EWMH: `_NET_WM_WINDOW_TYPE` → NSWindow style (DIALOG/TOOLBAR/UTILITY/MENU/TOOLTIP/SPLASH/NORMAL). `_NET_WM_STATE` MODAL/FULLSCREEN. `_NET_FRAME_EXTENTS` set proactively on MapWindow. WM minimum size floor: top-level windows below 200×100 enlarged in C++ CreateWindow. Pre-registered atoms 78-92.

- **Vivado banner + menu fixes** (v1.12.2): Deferred show for floor-sized windows (`pendingNonORShow` — show triggered by applyX11Resize, first present, or 500ms timeout). ConfigureNotify on user window drag (`WindowMoved` HostCmdType → fixes Java/Swing stale root coordinates after cross-monitor window move). mapWindow geometry sync (query X11 geometry before makeKeyAndOrderFront). **Vivado confirmed working** — full GUI with menus, dialogs, banner on multi-monitor setup.

- **Vitis extension stubs** (v1.13.0): XFIXES minor 1 (ChangeSaveSet) and minor 2 (SelectSelectionInput) void handlers. RANDR minor 15 (GetOutputProperty) reply-bearing handler returning empty property. Eliminates BadRequest errors from Vitis startup.

- **Multi-monitor window placement fix** (v1.13.0): `adjustNonOROriginForMainScreen()` places default-position (0,0) windows on `NSScreen.main` instead of virtual desktop top-left. Explicitly positioned windows unaffected. `deferredShowRetry()` polls up to 3s for floor-sized windows to get real geometry before showing.

- **Dynamic View menu** (v1.13.0): View menu moved from SwiftUI CommandMenu to AppDelegate NSMenu with NSMenuDelegate. "Show Log Window" ↔ "Hide Log Window" toggle updates dynamically via `menuNeedsUpdate()`. Status bar menu item also toggles.

- **Graceful quit with connected clients** (v1.13.0): `applicationShouldTerminate` calls `x11_stop_server()` before window teardown, replies after 100ms delay for server thread to exit. Prevents beach-ball hang when quitting while X11 clients are connected.

- **App icon** (v1.13.2): Custom icon from SwiftX11 logo PDF, all 10 asset catalog sizes (16–1024px) with blue background. Bundle ID changed to `com.rlan.SwiftX11` (reverse-DNS convention, also fixes poisoned macOS icon cache from old `RLAN.SwiftX11` ID).

- **NSWindow.sharingType** (v1.13.2): Set to `.readWrite` on all X11 windows for window server capture compatibility.

- **XInput2 extension** (v1.14.0→v1.15.4): Full XI2 event delivery infrastructure. `XI2EventMask.hpp` defines event types, masks, wire sizes. `EventOps.cpp` has 6 XI2 event senders (Motion, Button, Key, Crossing, Focus, RawMotion). `ExtensionOps.cpp` handles XIQueryVersion (v2.0), XISelectEvents (per-window + root masks), XIQueryDevice (4 virtual core devices), XIQueryPointer, XIGetClientPointer. 13 injection points send XI2 events alongside core events. XInputExtension advertised with `first_event=93` (range 93-109, above RANDR 89-90). Total advertised extensions: 10 (BIG-REQUESTS, RENDER, XFIXES, RANDR, XINERAMA, GE, SHAPE, XC-MISC, XInputExtension, XTEST).

- **XI2 RawMotion global delivery** (v1.15.3): RawMotion sent in `postMotion()` before `if (!deliver) return` check, so `GlobalPointerTracker`'s global mouse monitor (NSEvent global+local + 30fps timer) delivers RawMotion even when cursor is outside X11 windows. Required for xeyes pupil tracking. RawMotion events are 68 bytes with 2-axis valuator data (length=9).

- **XTEST extension** (v1.14.0): Synthesizes keyboard/mouse events via `x11_post_pointer_move2` / `x11_post_button` / `x11_post_key` bridge functions. FakeInput (minor 2) with types 2-6 (KeyPress/Release, ButtonPress/Release, MotionNotify). GetVersion (minor 0) returns v2.2.

- **Window Shape AA compositing** (v1.15.5): Straight alpha blending for SHAPE extension windows. Metal pipeline uses `sourceAlpha/oneMinusSourceAlpha`. Previous premultiplied attempt (v1.15.0) reverted due to double-application artifacts.

- **Vivado clipboard fix** (v1.15.17–v1.15.22): Five-bug chain fix for Edit→Copy: (1) Missing FocusIn/FocusOut on UngrabKeyboard (v1.15.17) — Java AWT blocked waiting for focus events. (2) Missing PropertyNotify on ChangeProperty (v1.15.18) — Java needed server timestamp from property event. (3–4) Proactive clipboard capture on SetSelectionOwner (v1.15.19–v1.15.20) — server requests selection content and pushes to NSPasteboard. (5) PropertyNotify gated by PropertyChangeMask (v1.15.22) — prevents event flooding during startup. Also: MotionNotify coalescing in HostCommandQueue (v1.15.15) prevents AWT lock starvation from queued motion events.

- **SubstructureRedirect emulation** (v1.17.0–v1.17.1): Replaces timer-based deferred show with proper WM emulation. Clients like Java AWT create windows at 1×1 then configure to real size — without SubstructureRedirect, MapWindow fires before ConfigureWindow arrives. Three-layer fix: (1) **Poll loop drain**: `readAndDispatch` returns `DispatchResult` enum; poll loop drains ALL buffered data per client before flushing pending maps. (2) **Deferred map**: MapWindow on tiny (<50px) root children calls `ctx.addPendingMap(wid)` instead of `x11_ui_push_map`. `flushPendingMaps()` called after drain. (3) **Peak pre-map size tracking**: Tracks largest ConfigureWindow size seen per unmapped root child. At flush time, resolution order: WM_NORMAL_HINTS → peak size → current geometry. Centers resized windows on primary monitor. Also fixes: kWM_NORMAL_HINTS atom constant (41→40), double MAP_SHOW from MapSubwindows+MapWindow (`wasMapped` guard), CreateWindow size floor removed (stored as-is, WM emulation at map time). `pushMapExtras()` helper sends resize/move/_NET_FRAME_EXTENTS after map.

- **Phase 8 Protocol Hardening** (v1.18.0–v1.19.14):
  - **Event delivery audit** (v1.18.1): Comprehensive fix for event delivery across ~50 handlers.
  - **XCB sequence desync fix** (v1.18.2): Replace `nextEventSeq()` with `lastSeq()`.
  - **IconicState fix** (v1.18.3): Fix sliding window glitch on Vivado startup.
  - **Dialog title fix** (v1.19.6): Child windows (FocusProxy, Content window) no longer overwrite parent's WM_NAME. Only top-level windows (wid == host) push title updates.
  - **Dispatch ring buffer** (v1.19.5): 16-entry ring buffer in ClientSession recording every dispatched request (major, minor, seq, reply_sent). Dumped on client disconnect for crash diagnosis.
  - **Wire ring buffer** (v1.19.9): 32-entry ring buffer in XProtoTransport recording every outgoing packet header. Dumped on client disconnect.
  - **Wire trace toggle** (v1.19.10): Atomic flag `g_wire_trace` controlled by UI toggle "Wire Trace (stderr)", gates live fprintf of every incoming request and outgoing packet to Xcode console.
  - **JidePopup close fix** (v1.19.12): Windows without WM_DELETE_WINDOW now receive UnmapNotify + DestroyNotify instead of force-disconnecting the entire client. Prevents Vivado crash when closing JidePopup windows.
  - **XTEST 2.0 safe mode** (v1.19.14): XTEST GetVersion reports v2.0 (not 2.2) to prevent GrabControl-triggered crash. CompareCursor stub added (returns same=true). FakeInput and GrabControl silently consumed. Full XTEST 2.2 spec coverage implemented but version downgraded due to unresolved crash.

### XTEST / GrabControl Crash Investigation (v1.19.x)

**Status**: Root cause isolated but not yet fixed.

**The crash**: Vivado segfaults (signal 11) ~58ms after calling XTEST GrabControl (minor 3). Crash occurs in `gdk_display_manager_get_default_display`. Only happens when XTEST reports v2.2 (which enables GrabControl). Reporting v2.0 prevents the crash entirely.

**What has been ruled out**:
- AT-SPI / DBus: Crash occurs even with dbus running and NO_AT_BRIDGE=0
- Malformed XTEST replies: GetVersion reply is byte-perfect per spec
- Sequence desync: No SEQ_FLOOR bumps observed
- Interleaved events: Suppressing flushNotifyQueue after XTEST still crashes

**What is known**:
- GrabControl(impervious=true) is a void request (no reply) — our no-op stub consumes it
- The crash happens AFTER GrabControl returns, not during
- XQuartz handles XTEST 2.2 + GrabControl without issues
- The client may expect grab-impervious state to be set, affecting subsequent grab operations

**Next investigation**: What does GrabControl actually do in Xorg/XQuartz? Does it modify grab state that affects subsequent operations? The no-op stub may leave inconsistent state.

**Workaround**: Report XTEST v2.0 — client never calls GrabControl, no crash.

### Known Issues (v1.19.14)
- **JidePopup decorated windows**: JidePopup windows appear as full decorated NSWindows with title bars instead of borderless popups. Need to honor `_NET_WM_WINDOW_TYPE` / `_MOTIF_WM_HINTS` for undecorated windows. (HIGH priority)
- **Ctrl+click regression**: Ctrl+click no longer triggers context menus (button 3) in Vivado. Two-finger trackpad click works. Regression in v1.17→v1.19 range. (MEDIUM)
- **Pointer offset after left-edge resize**: Menu selections offset horizontally after resizing window by dragging left edge. Likely stale origin in coordinate transform. (MEDIUM)
- **xcalc -rpn extra button labels**: Client-side issue, same on XQuartz.
- **xclock/xcalc FontSet warnings**: Xlib XCreateFontSet() charset coverage issue.
- **Vivado menu highlighting**: Sluggish; needs xscope comparison with XQuartz.

### Next Major Tasks
See `docs/TODO.md` for the comprehensive roadmap. Priority order:
1. **GrabControl crash root cause** — Investigate what GrabControl does in Xorg/XQuartz
2. **JidePopup windows** — Honor window type hints for borderless popups
3. **Ctrl+click regression** — Identify which change broke it
4. **Vitis testing** — Eclipse SWT/GTK from ALMA 9 container
