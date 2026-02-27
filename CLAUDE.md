# SwiftX11 — X11 Server for macOS

## Project Overview

SwiftX11 is an X11 protocol server running natively on macOS, implementing the X11 wire protocol so that X11 clients (xterm, xeyes, etc.) can display and interact via native Cocoa/Metal windows.

## Architecture

### Language Layers
- **Swift** (`macos/SwiftX11/`): UI owner — AppKit windows, Metal/software rendering, surface allocation, damage scheduling, networking (accepts X11 connections)
- **C++** (`macos/X11LowLevel/cpp/X11Protocol/`): Protocol core — request parsing, reply/event framing, raster drawing ops, resource tables (windows, pixmaps, GCs, fonts, atoms, colormaps)
- **C** (`macos/X11LowLevel/src/`): Legacy shim layer being eliminated — request queue, bridging. C framebuffer infrastructure is dead code (no callers remain)

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
Swift main thread → ensureHostSurface → x11_surface_update → x11_requests_push_set_presentable
SetPresentable → setPresentable + fillWindowBackgroundIfReady + sendExposeSubtree
  (This is when backgrounds actually get painted and children get valid Expose)
```

### Surface Resize Re-expose
When `x11_surface_update` detects a surface size change (e.g., initial 64×64 → real 819×484), it queues a `SurfaceResized` host command via `x11_proto_bridge_surface_resized(hostXid)`. The handler calls `sendExposeSubtree` + marks dirty + pushes damage, ensuring all mapped children get re-exposed at the correct geometry. This fixes a timing race where `setContentSize` is deferred via `DispatchQueue.main.async` but surface registration happens at the NSWindow's default (small) size.

## Key Files

| File | Role |
|------|------|
| `SwiftX11/UI/Windows/X11WindowHost.swift` | Host window, surface allocation, Metal/software rendering |
| `SwiftX11/Core/WindowRegistry.swift` | Cocoa window management, noteX11WindowCreated, mapWindow |
| `X11LowLevel/cpp/X11Protocol/include/Core/SurfaceDesc.hpp` | Surface descriptor struct |
| `X11LowLevel/cpp/X11Protocol/include/Core/DrawableSurfaceRegistry.hpp` | Thread-safe XID→surface registry |
| `X11LowLevel/cpp/X11Protocol/include/Core/DrawableRW.hpp` | Unified drawable abstraction (w, h, stridePixels, pixels32) |
| `X11LowLevel/cpp/X11Protocol/src/Core/DrawableRW.cpp` | resolveDrawableRW: Swift surface only, child→host offset routing |
| `X11LowLevel/cpp/X11Protocol/src/Ops/DrawOps.cpp` | PutImage, CopyArea, ClearArea, text ops |
| `X11LowLevel/cpp/X11Protocol/src/Ops/ShapeOps.cpp` | PolyArc, PolyFillArc, PolyFillRectangle, PolyLine, PolySegment, PolyPoint |
| `X11LowLevel/cpp/X11Protocol/src/Ops/WindowOps.cpp` | CreateWindow, MapWindow, ConfigureWindow, rootless resize |
| `X11LowLevel/cpp/X11Protocol/src/XProtoServerBridge.cpp` | Host command dispatch (SetPresentable, SurfaceResized, RootlessResize), sendExposeSubtree |
| `X11LowLevel/cpp/X11Protocol/src/SurfaceBridge.cpp` | Surface registration, size-change detection → SurfaceResized |
| `X11LowLevel/cpp/X11Protocol/src/Utils/Damage.hpp` | damageOrDirty() helper — translates child→host, writes shared accumulator + signals |
| `X11LowLevel/cpp/X11Protocol/src/UI/UICommandQueue.cpp` | UI command queue + shared damage accumulator (x11_shared_damage_union/consume/clear) |
| `X11LowLevel/src/x11_requests.c` | Request queue (C), coalescing, rootless resize bridging |
| `SwiftX11/Core/XServerController.swift` | Server startup, version banner |
| `SwiftX11/UI/Windows/X11MetalRenderer.swift` | Metal texture management, partial sub-rect uploads |
| `docs/TODO.md` | Comprehensive project roadmap |

## Build & Run

- Xcode project: `macos/SwiftX11.xcodeproj`
- Build target: SwiftX11 (macOS app)
- Test clients: `xeyes`, `xterm` connected via `DISPLAY=:0`
- Scrollbar test: `xterm -sb -rightbar -bc`

## Development Guidelines

### Drawing Operations
- Always resolve drawables via `resolveDrawableRW(ctx, drawable, dst)` before accessing pixels
- Use `dst.stridePixels` (not `dst.w`) for row-to-row stepping in all pixel indexing
- Force alpha opaque (`| 0xFF000000u`) for XRGB8888 surfaces
- Call `damageOrDirty(ctx, drawable)` after modifying window pixels
- PolyFillRectangle is the reference pattern for correct stride-aware rasterization

### Surface Lifecycle
- Surfaces are keyed by **host (top-level) window XID** in the registry
- Child windows resolve to host surface + offset via `computeOffsetInHost`
- `generation` counter tracks reallocations; C++ can detect stale pointers
- On window destruction, `x11_surface_clear(xid)` removes the registry entry

### Debugging
- **Two trace tiers**:
  - `#ifndef NDEBUG` (debug builds): Key lifecycle/diagnostic traces that are always on in debug — `[LIFECYCLE]`, `[BG_FILL]`, `[BG_FILL_RETRY]`, `[EXPOSE_SUBTREE]`, `[SET_PRESENTABLE]`, `[SURFACE_UPDATE]`, `[SURFACE_RESIZED]`
  - `#ifdef X11_TRACE_VERBOSE`: High-frequency per-op traces gated behind a separate flag — `[RESOLVE]`, `[COPY_SURFACE]`, `[DAMAGE]`, `[DISPATCH]`, `[FOCUS]`, `[BTN]`, `[SCROLL]`, `[KEY]`, `[TEXT]`, `[CopyArea]`, `[ClearArea]`, `[CROSS]`, `[SEND]`, plus all `ctx.tracef()` calls (~71 call sites silenced at once)
- To enable verbose tracing: add `-DX11_TRACE_VERBOSE` to OTHER_CPLUSPLUSFLAGS in Xcode build settings
- Watch for stride vs width mismatches — the most common class of rendering bug
- **Version banner**: `SwiftX11 v{version}` printed at startup (Swift `XServerController.buildVersion` + C++ `kSwiftX11Version`). Bump version when making changes to verify the correct build is running.

### Current State (v0.4.3)
- `resolveDrawableRW` is Swift-surface-only (no C FB fallback)
- Host windows resolve directly to their Swift surface
- Child windows resolve to host surface + computed offset (with negative offset clamping)
- Present path copies the single host surface (no compositing)
- `fillWindowBackgroundIfReady` in `sendExposeSubtree` retries background fills after SetPresentable
- `SurfaceResized` mechanism re-exposes children when surface dimensions change
- **Damage rects threaded end-to-end**: C++ draw ops report precise rects → shared accumulator → Metal partial texture upload
- **Shared damage accumulator**: `x11_shared_damage_union/consume` in UICommandQueue.cpp — mutex-protected, 64-entry fixed array, bypasses UI command queue drain latency
- **Metal partial uploads**: `X11MetalRenderer.updateTexture()` uses `MTLTexture.replace(region:)` for sub-rect uploads; `fullUploadCountdown` forces full uploads for first 3 frames after texture creation
- **Dead code removed** (v0.4.3): `WindowTable::DirtyRect/markDirtyRect/consumeDirtyRectIfReady`, `X11_REQ_DAMAGE/x11_requests_push_damage/x11_server_emit_window_damage`, `UICommand::Type::Damage` handler, Swift `DirtyRectU/dirtyByHostU/noteDamageRect/handleDamageRect/hostSizeU`
- xterm with scrollbar (`xterm -sb -rightbar -bc`) works correctly
- xeyes works correctly
