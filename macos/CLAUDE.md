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
| `X11LowLevel/cpp/X11Protocol/src/Ops/EventOps.cpp` | MotionNotify, ButtonPress/Release, FocusIn/Out, crossing events |
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
| `X11LowLevel/cpp/X11Protocol/include/Core/PropertyTable.hpp` | Shared property storage (used by PropOps + SelectionOps) |
| `X11LowLevel/cpp/X11Protocol/include/Core/ClipboardAtoms.hpp` | Well-known atom constants (CLIPBOARD, TARGETS, UTF8_STRING, etc.) |
| `X11LowLevel/cpp/X11Protocol/src/Ops/SelectionOps.cpp` | Selection protocol + macOS clipboard bridge |
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

### Current State (v1.5.0)
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
- **Extension stubs** (v1.4.0): Handler code for XFIXES (134, v5.0), SHAPE (135, v1.1), RANDR (136, v1.5), Xinerama (137, v1.1 + IsActive + QueryScreens), GE (138, v1.0). Opcodes defined in X11ExtOpcodes.hpp. **NOT advertised to clients** — QueryExtension returns present=0. Only BIG-REQUESTS is advertised. Advertising extensions with incomplete operations causes clients to take broken code paths (xeyes+SHAPE=broken rendering, xeyes+RENDER=blank window).
- **RENDER extension** (v1.4.0, major 139): Handler code for QueryPictFormats (ARGB32/RGB24/A8/A4/A1), CreatePicture/FreePicture, Composite (PictOpSrc/Over/Add/Clear), FillRectangles, CreateSolidFill, QueryFilters. Glyph ops are stubs (consume silently). Picture table tracks drawable/format/solid state. **NOT advertised to clients** — needs Trapezoids + CompositeGlyphs rendering before enabling.
- **Selections/clipboard bridge** (v1.5.0): Pre-registered atoms 69-77 (CLIPBOARD, TARGETS, UTF8_STRING, TIMESTAMP, TEXT, MULTIPLE, INCR, WM_PROTOCOLS, WM_DELETE_WINDOW). PropertyTable extracted to shared header. Swift registers get/set callbacks via `x11_clipboard_register()`. ConvertSelection reads NSPasteboard when no X11 owner. SendEvent intercepts SelectionNotify for CLIPBOARD → NSPasteboard write. TARGETS returns supported types.
- **GC fill-style rendering** (v1.5.0): FillSolid/FillTiled/FillStippled/FillOpaqueStippled implemented in PolyFillRectangle, FillPoly, PolyFillArc via `Utils/FillStyle.hpp` helper. Tile pixmaps sampled at `(px-ts_x_origin)%tile_w`. Stipple bitmaps bit-tested with proper modulo wrapping. Solid fill fast path preserved (zero overhead when fill_style==0).
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

### Known Issues (v1.5.0)
- **xcalc wrong characters**: sqrt sign and some other special characters display incorrectly — Phase 3 font/encoding issue.
- **xclock/xcalc FontSet warnings**: "Missing charsets in String to FontSet conversion" — Phase 3 font infrastructure issue. Xlib's XCreateFontSet() expects multiple charset fonts; SwiftX11 has minimal BDF coverage.
- **xterm occasional uncleared pixels at bottom**: Stale pixels visible at bottom of xterm window in some cases. May be a damage rect or ClearArea issue.
- **Window close (red button) does not kill client**: Closing the Cocoa window hides the NSWindow but the X11 client process keeps running. Need WM_DELETE_WINDOW ClientMessage support (ICCCM) or forceful client disconnect on window close.
- **RENDER glyph rendering**: CompositeGlyphs8/16/32 and AddGlyphs are stubs — need actual glyph bitmap storage/rendering for anti-aliased fonts.

### Next Major Tasks (Vivado/Vitis Roadmap)
See `docs/TODO.md` for the comprehensive 5-phase plan with testing apps per phase. Priority order:
1. **Window close → client kill** — Red button should terminate X11 client (WM_DELETE_WINDOW or socket close)
2. **Error handling** — proper X11 error generation (BadWindow, BadDrawable, etc.)
3. **Enable RENDER extension** — Complete Trapezoids + CompositeGlyphs, then advertise
4. **Enable remaining extensions** — SHAPE, XFIXES, RANDR, Xinerama, GE one at a time
5. **Container networking** — TCP + Unix socket for Docker workflow
