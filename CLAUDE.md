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

**Known issue (active debugging)**: xterm scrollbar child window does not render. The VT (text) child window works. The child→host routing, Expose delivery, and background fill all appear structurally correct but the scrollbar remains blank. See "Current Debugging Focus" below.

### Damage/Present Pipeline
```
C++ draw op → damageOrDirty(ctx, drawable)
  → routes to host window (topLevelAncestorOf)
  → x11_requests_push_damage(host) or markDirty(host)
  → Swift: WindowRegistry.noteDamageRect() → schedulePresent()
  → X11View renders frame (software blit or Metal)
```

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
| `X11LowLevel/cpp/X11Protocol/src/XProtoServerBridge.cpp` | Host command dispatch (SetPresentable, RootlessResize), sendExposeSubtree |
| `X11LowLevel/cpp/X11Protocol/src/Utils/Damage.hpp` | damageOrDirty() helper |
| `X11LowLevel/src/x11_requests.c` | Request queue (C), coalescing, rootless resize bridging |
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
- Debug builds (`#ifndef NDEBUG`) have extensive trace logging
- Key log prefixes: `[RESOLVE]`, `[BG_FILL]`, `[BG_FILL_RETRY]`, `[EXPOSE_SUBTREE]`, `[SET_PRESENTABLE]`, `[COPY_SURFACE]`, `[DAMAGE]`
- `damageOrDirty` logs routing decisions in debug mode
- Watch for stride vs width mismatches — the most common class of rendering bug

### Current State (post C-FB elimination)
- **C framebuffers are dead code**: No callers remain in C++ protocol layer. `g_fb[]`, `x11_backend_fb_*` functions, `x11_backend_fb.h` can be deleted.
- `resolveDrawableRW` is Swift-surface-only (no C FB fallback)
- Host windows resolve directly to their Swift surface
- Child windows resolve to host surface + computed offset
- Present path copies the single host surface (no compositing)
- `fillWindowBackgroundIfReady` in `sendExposeSubtree` retries background fills after SetPresentable

## Current Debugging Focus: xterm Scrollbar

### Problem
`xterm -sb -rightbar -bc` — the scrollbar child window does not render. Text (VT child) works fine.

### What's been tried
1. **background_pixel support** — Added to WindowTable/WindowView, applied in MapWindow and ClearArea. Did not fix scrollbar.
2. **Hybrid compositing** — blitCFramebuffersToSwiftSurface approach. Rejected, removed.
3. **C FB elimination** — Removed all C FB usage; child windows now draw into host surface at offset. Text works but scrollbar still blank.
4. **fillWindowBackgroundIfReady in sendExposeSubtree** — Ensures backgrounds are painted when surface is ready (not just at MapWindow time). Still no scrollbar.

### Architecture analysis (all appears correct)
- `descendantsOf(host)` finds scrollbar as mapped child ✓
- `sendExposeSubtree` sends Expose to scrollbar ✓
- `resolveDrawableRW(scrollbar)` routes to host surface + offset ✓
- `damageOrDirty(scrollbar)` routes damage to host ✓
- Present copies entire host surface ✓

### Next debugging steps to try
- **Capture and analyze stderr logs**: Run debug build and search for `[RESOLVE]` / `[EXPOSE_SUBTREE]` / `[BG_FILL_RETRY]` lines for the scrollbar XID. Verify the scrollbar window exists, gets Expose events, and that resolveDrawableRW succeeds with valid offset/dimensions.
- **Check if xterm actually sends draw ops for the scrollbar**: The scrollbar might use X11 ops we don't handle, or might not draw at all if it thinks the scrollbar isn't visible.
- **Check surface size vs X11 geometry**: If the Swift surface is smaller than the X11 window geometry (e.g., during initial sizing), the scrollbar offset might be out of bounds, producing effW=0 or effH=0.
- **Compare scrollbar XID behavior vs VT XID behavior** in logs: Since VT works but scrollbar doesn't, find the divergence point.
- **Inspect Swift-side WindowRegistry**: Does `noteX11WindowCreated` for child windows properly track the scrollbar? Does `mapWindow` for children do anything that could block rendering?
