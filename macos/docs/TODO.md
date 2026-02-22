SwiftX11 TODO

Last updated: 2026-02-22

⸻

0️⃣ Architecture Pivot (NOW)

Current status (bring-up progress)
  •  ✅ Multi-client ID space plumbed: per-connection rid_base/rid_mask advertised in SetupSuccess and stored in transport.
  •  ✅ UICommandQueue migrated to generic `UICommand` + `push(const UICommand&)` (still backed by x11_requests.* for now).
  •  ✅ Swift host surfaces: Swift allocates persistent 32bpp CPU buffers per host window and publishes them via `x11_surface_update/clear`.
  •  ✅ SurfaceRegistry: C++ prefers Swift surfaces for WINDOW drawables and carries stride (`stridePixels`) through `DrawableRW`.
  •  ✅ Alpha/opacity: surfaces initialized opaque in Swift; raster fills force opaque alpha.
  •  ✅ Drawing migrated to SurfaceRegistry where needed: PutImage(ZPixmap→WINDOW), ClearArea, ImageText8, PolyText8.
  •  ✅ Metal + software parity sanity-checked during bring-up.

Goal
  •  Eliminate the C layer entirely: only C++ (core protocol + raster) and Swift (UI + networking).
  •  Remove single-client globals: support multiple concurrent clients cleanly.
  •  Make Swift the owner of all WINDOW backing stores (host + child) now.

Swift-owned WINDOW surfaces
  •  Swift allocates/resizes per-host WINDOW backing stores (persistent CPU buffer today; can later be backed by MTLBuffer/MTLTexture).
  •  C++ draws into Swift-provided surfaces via `SurfaceDesc { ptr, bytesPerRow, w, h, format, generation }` (C++ never owns/presents window framebuffer memory).
  •  Swift publishes surfaces via temporary shims `x11_surface_update(hostXid, ...)` / `x11_surface_clear(hostXid)`; later replace with Swift C++ interop calls.
  •  Important: bytesPerRow may be padded; draw code must respect stride (no `row = y*w` assumptions).

Surface registry (C++ API consumed by Swift)
  •  Implemented: `DrawableSurfaceRegistry` keyed by XID (currently used for host WINDOW drawables).
  •  Implemented: `updateSurface/clearSurface` entrypoints (currently reached via `x11_surface_update/clear` shims).
  •  Implemented: `DrawableRW` now includes `stridePixels`; `resolveDrawableRW` prefers Swift surfaces for windows, with temporary fallback to C framebuffer access.
  •  Next: route child window drawables to host surfaces with origin offsets; remove all remaining C framebuffer entrypoints.

Damage / present (Swift-driven)
  •  C++ reports damage as rects per host/top-level window (no “always damage” hacks).
  •  Swift unions rects and schedules exactly one present per host per runloop tick.

Multi-client (no globals)
  •  Introduce `XServer` as an instance with owned registries (resources, atoms, colormaps, fonts, drawables).
  •  Each connection is a `Client` object: byteOrder, seq, idBase/idMask, transport, pending replies/events.
  •  No global “current client” pointers; dispatcher always routes via `(XServer&, Client&)`.

C removal plan
  •  Rename/migrate remaining `.c` backend files to C++ (`.cpp`) and delete `x11_backend_*` C APIs.
  •  Swift calls into C++ via Swift C++ Interop (module map / clang module) rather than `@_cdecl` shims.
  •  Keep the language boundary one-way: Swift drives the loop and invokes C++ (avoid callbacks from C++ into Swift).

⸻

1️⃣ Core Protocol Correctness

Wire / framing
  •  Endianness: ByteReader/ReplyWriter must respect client byte order (swap for big-endian clients).
  •  Audit all reply paths for consistent framing (seq, lenw, padding, no stray sendAll chunks).
  •  Centralize reply header writing (avoid duplicated reply logic).

Opcode coverage
  •  Finish remaining core opcodes encountered by xterm.
  •  Implement cursor ops (93–96) fully.
  •  Verify QueryBestSize (97) semantics.

GC semantics
  •  Implement GC function (GXcopy, GXxor, etc.).
  •  Apply GC planemask in all draw paths.
  •  Enforce GC clipping region.

⸻

2️⃣ Drawing / Raster Semantics

CopyPlane
  •  Support CopyPlane from depth>1 sources (use bitPlane mask).
  •  Apply GC function + planemask.
  •  Emit proper X11 errors (BadDrawable/BadGC/BadMatch).
  •  Use actual bitmapBitOrder from setup instead of hardcoded LSBFirst.
  •  Improve damage region precision (avoid full-host damage).
  •  if src is a WINDOW, implement plane extraction from 32bpp framebuffer using bitPlane + planemask + GC.function.


PutImage
  •  Remove temporary routing hacks once full rootless model is stable.
  •  ✅ Window destinations via Swift SurfaceRegistry: ZPixmap depth 24/32 (32bpp on wire) supported; keep pixmap XY1 path.
  •  Support depth conversions where required.

General drawing
  •  Implement proper error generation (BadDrawable, BadGC, BadMatch, BadValue).
  •  Validate depth compatibility rules between src/dst.
  •  ✅ ClearArea migrated to SurfaceRegistry (`resolveDrawableRW` + stridePixels).
  •  ✅ Text drawing migrated: ImageText8 + PolyText8 draw via BDF glyph bitmaps (MSBFirst within bytes) and SurfaceRegistry.

PolyFillRectangle
  •  Apply GC function / planemask / fill-style (right now this is “solid fg overwrite” only).
  •  Support clip mask / clip rectangles from GC.

PolyFillArc
  •  Implement GC function/planemask/clip mask support for arc fill.
  •  Use fixed-point / integer rasterization (avoid float + atan2 for speed).
  •  Improve damage region precision (rect of arc bounds or bbox intersection).
  •  Handle a2==0 (no-op) and negative extents exactly per spec.

PolyArc
  •  Implement lineWidth (GC) and proper stroke rasterization (not just “ring within eps”).
  •  Implement GC clip mask/clip rectangles.
  •  Implement GC function + planemask.
  •  Improve arc precision: switch from float/atan2 to fixed-point stepping or midpoint ellipse algorithm.
  •  Damage region: compute bbox of stroke (x/y/w/h) and use rect-based UI_DAMAGE later.

ConfigureWindow
  •  Support stack-mode / sibling / border-width fields (bits 4..7) or at least validate/ignore safely.
  •  Emit correct X11 errors (BadWindow / BadValue) when wid doesn’t exist or values are malformed.
  •  Swift owns WINDOW backing stores (host + child). C++ updates geometry/state only.
  •  On map/resize, Swift (re)allocates surfaces and calls `server.updateSurface(xid, SurfaceDesc)`.
  •  Continue sending ConfigureNotify to child even when clamped to host (rootless cascade per ICCCM).

⸻

3️⃣ Fonts (xterm bring-up → real implementation)
  •  ✅ Bring-up text ops: ImageText8 + PolyText8 implemented enough for xterm; still needs full spec coverage (PolyText16/ImageText16, full item semantics, GC clip/rop correctness).
  •  Complete FontOps so xterm runs without fallback hacks.
  •  Ensure QueryFont / QueryTextExtents / ListFonts / ListFontsWithInfo match Xproto.h exactly.
  •  Return correct font properties (SPACING, AVERAGE_WIDTH, etc.).
  •  Replace bitmap-only stub logic with real font abstraction layer.
  •  Long-term: integrate CoreText-backed scalable fonts.

⸻

4️⃣ Colormaps / Colors
  •  Complete ColorOps cluster (78–92):
  •  AllocColor
  •  AllocNamedColor
  •  AllocColorCells
  •  AllocColorPlanes
  •  FreeColors
  •  StoreColors
  •  LookupColor
  •  Ensure QueryColors request parsing matches Xproto.h exactly.
  •  Add proper colormap storage instead of implicit 24-bit unpack.

⸻

5️⃣ Damage & Present Pipeline (Rootless Architecture)

Current bring-up hacks to remove
  •  Remove temporary “always damage” routing.
  •  Eliminate unconditional push_damage workarounds.

Final routing model
  •  pixmap ops → never enqueue present
  •  child window ops → route to top-level host
  •  top-level ops → damage self
  •  Ensure `server.queueDamage(host, rects)` always results in exactly one UI_DAMAGE per host per runloop tick`

Dirty/presentable gating
  •  Validate consumeDirtyIfReady semantics.
  •  Ensure SetPresentable triggers one present if dirty.
  •  Remove race between ROOTLESS_RESIZE and snapshot.

Damage precision
  •  Pass rects instead of full-window damage.
  •  Implement rect unioning on Swift side.

⸻

6️⃣ Rootless Resize Model
  •  Remove suppression complexity once stable.
  •  Validate child clamp logic against ICCCM expectations.
  •  Ensure windowResized never echoes infinite resize loops.
  •  Separate logical X11 geometry from Cocoa pixel size cleanly.

⸻

7️⃣ Swift UI / AppKit Stability
  •  Eliminate remaining layout recursion triggers.
  •  Ensure setContentSize never runs inside layout.
  •  ✅ Metal + software parity sanity-checked (surfaces + stride + opacity).
  •  Ensure toggling Metal works:
  •  before client launch
  •  mid-session
  •  during resize
  •  Stabilize MTKView attach timing (drawableSize lifecycle).

⸻

8️⃣ Debug & Instrumentation
  •  Standardize logging categories (PROTO, REPLY, EVENT, DAMAGE, PRESENT, RESIZE).
  •  Add global debug level switch.
  •  Add server-side counters for:
  •  push_damage enqueue count
  •  drain count
  •  present count
  •  Add Swift framebuffer inspection tools (nonwhite scan already started).

⸻

9️⃣ Cleanup / Refactor
  •  Remove temporary SurfaceRegistry shims (`x11_surface_update/clear`) once Swift C++ interop is in place.
  •  Remove remaining C framebuffer fallback (`x11_xproto_window_fb_rw`) after child→host routing is implemented.
  •  Delete remaining C backend entrypoints (`x11_backend_*`) after migrating callers to C++/Swift surface registry.
  •  Remove obsolete handler registrations (QueryColors duplicates, etc.).
  •  Consolidate opcode constants (use x11::opcode::* everywhere).
  •  Remove remaining hardcoded numerics.
  •  Consolidate damageOrDirty into single canonical implementation.
  •  Separate C “backend” from Xproto logic more cleanly.

⸻

🔟 Future Architecture (Long-Term)
  •  Separate compositing from protocol (allow offscreen composition layer).
  •  Add extension framework (XInput, RANDR, etc.).
  •  Keep Swift as compositor/UI + networking; keep C++ as protocol+raster core; continue shrinking the boundary (no C).
  •  Add basic ICCCM compliance (WM_HINTS, size hints, etc.).
