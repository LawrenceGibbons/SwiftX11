# SwiftX11 — X11 Server for macOS

## Project Overview

SwiftX11 is an X11 protocol server running natively on macOS, implementing the X11 wire protocol so that X11 clients (xterm, xeyes, etc.) can display and interact via native Cocoa/Metal windows.

## Architecture

### Language Layers
- **Swift** (`macos/SwiftX11/`): UI owner — AppKit windows, Metal/software rendering, surface allocation, damage scheduling, networking (accepts X11 connections)
- **C++** (`macos/X11LowLevel/cpp/X11Protocol/`): Protocol core — request parsing, reply/event framing, raster drawing ops, resource tables (windows, pixmaps, GCs, fonts, atoms, colormaps)
- **C** (`macos/X11LowLevel/src/`): Legacy shim layer being eliminated — backend framebuffer, request queue, bridging

### Key Design: Swift-Owned Surfaces
Swift allocates all WINDOW backing stores (host-level CPU buffers). C++ draws into these via `DrawableSurfaceRegistry`:

```
Swift: ensureHostSurface(wPx, hPx)
  → x11_surface_update(xid, ptr, bytesPerRow, w, h, generation)
    → DrawableSurfaceRegistry::set(hostXid, SurfaceDesc)
      → C++ drawing ops resolve via resolveDrawableRW()
```

**Critical**: `bytesPerRow` is 64-byte aligned, so `stridePixels = bytesPerRow/4` often differs from `width`. All pixel row indexing MUST use `dst.stridePixels`, never `dst.w`.

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
    → FB_RESIZE, geometry update, ConfigureNotify + Expose to client
    → damageOrDirty()
```

## Key Files

| File | Role |
|------|------|
| `SwiftX11/UI/Windows/X11WindowHost.swift` | Host window, surface allocation, Metal/software rendering |
| `X11LowLevel/cpp/X11Protocol/include/Core/SurfaceDesc.hpp` | Surface descriptor struct |
| `X11LowLevel/cpp/X11Protocol/include/Core/DrawableSurfaceRegistry.hpp` | Thread-safe XID→surface registry |
| `X11LowLevel/cpp/X11Protocol/include/Core/DrawableRW.hpp` | Unified drawable abstraction (w, h, stridePixels, pixels32) |
| `X11LowLevel/cpp/X11Protocol/src/Core/DrawableRW.cpp` | resolveDrawableRW: prefers Swift surface, falls back to C FB |
| `X11LowLevel/cpp/X11Protocol/src/Ops/DrawOps.cpp` | PutImage, CopyArea, ClearArea, text ops |
| `X11LowLevel/cpp/X11Protocol/src/Ops/ShapeOps.cpp` | PolyArc, PolyFillArc, PolyFillRectangle |
| `X11LowLevel/cpp/X11Protocol/src/Ops/WindowOps.cpp` | CreateWindow, MapWindow, ConfigureWindow, rootless resize |
| `X11LowLevel/cpp/X11Protocol/src/Utils/Damage.hpp` | damageOrDirty() helper |
| `X11LowLevel/src/x11_requests.c` | Request queue (C), coalescing, rootless resize bridging |
| `X11LowLevel/src/x11_shim.c` | C shim: window lifecycle, FB access, damage routing |
| `docs/TODO.md` | Comprehensive project roadmap |

## Build & Run

- Xcode project: `macos/SwiftX11.xcodeproj`
- Build target: SwiftX11 (macOS app)
- Test clients: `xeyes`, `xterm` connected via `DISPLAY=:0`

## Development Guidelines

### Drawing Operations
- Always resolve drawables via `resolveDrawableRW(ctx, drawable, dst)` before accessing pixels
- Use `dst.stridePixels` (not `dst.w`) for row-to-row stepping in all pixel indexing
- Force alpha opaque (`| 0xFF000000u`) for XRGB8888 surfaces
- Call `damageOrDirty(ctx, drawable)` after modifying window pixels
- PolyFillRectangle is the reference pattern for correct stride-aware rasterization

### Surface Lifecycle
- Surfaces are keyed by **host (top-level) window XID** in the registry
- Child windows should resolve to host surface + offset (not yet fully implemented — currently falls back to C FB)
- `generation` counter tracks reallocations; C++ can detect stale pointers
- On window destruction, `x11_surface_clear(xid)` removes the registry entry

### Debugging
- Debug builds (`#ifndef NDEBUG`) have extensive trace logging
- `damageOrDirty` logs routing decisions in debug mode
- Watch for stride vs width mismatches — the most common class of rendering bug in the transition

### Current Transition State
The project is migrating from C-managed per-window framebuffers to Swift-owned shared host surfaces. During this transition:
- `resolveDrawableRW` has a fallback to `x11_xproto_window_fb_rw` for windows without Swift surfaces
- Some operations may still use the old C path
- The goal is to eliminate all C framebuffer code once child→host routing is complete
