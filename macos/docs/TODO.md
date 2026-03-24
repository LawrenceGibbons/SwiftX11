# SwiftX11 TODO

Last updated: 2026-03-24 (v1.19.25 — WM compliance, error cleanup, find bar, resize fix)

Target: Full support for Xilinx Vivado and Vitis (Java Swing + Eclipse SWT/GTK running from a Linux container).

---

## Completed

### Architecture
- [x] Swift-owned WINDOW surfaces (CPU buffers allocated by Swift, drawn by C++ via DrawableSurfaceRegistry)
- [x] Child window -> host surface routing with offset (resolveDrawableRW + computeOffsetInHost)
- [x] Negative offset clamping for child windows at negative positions
- [x] SurfaceResized re-expose mechanism for timing races
- [x] bytesPerRow 64-byte aligned; all draw code uses stridePixels
- [x] background_pixel stored in WindowTable, applied on MapWindow, used in ClearArea
- [x] CWBackPixmap (bit 0): ParentRelative resolves nearest ancestor's background_pixel; None clears background (v1.5.2)
- [x] clearBackground() and resolveParentRelativeBackground() in WindowTable (v1.5.2)

### Multi-Monitor Support (v1.9.7)
- [x] ScreenLayout cache with CGDisplayRegisterReconfigurationCallback auto-refresh
- [x] X11↔macOS coordinate conversion using virtual desktop union bounds (all screens)
- [x] Dynamic RANDR: per-monitor output/CRTC/mode from real CGDisplay data
- [x] Dynamic Xinerama: per-monitor screen entries from real CGDisplay data
- [x] GetGeometry on root returns actual virtual desktop dimensions
- [x] WarpPointer uses virtual desktop bounds (not just NSScreen.main)
- [x] Override-redirect window Metal drawable retry for cross-screen presentation
- [x] RANDR protocol compliance: RRGetCrtcTransform (minor 27), RRGetPanning (minor 28), RRSetPanning (minor 29), RRGetProviders (minor 32), correct minor opcode numbers, 36-byte RRGetOutputInfo reply

### Damage / Present Pipeline
- [x] Shared damage accumulator (x11_shared_damage_union/consume) bypasses UI queue latency
- [x] Metal partial texture upload via MTLTexture.replace(region:)
- [x] Damage rects threaded end-to-end: draw ops -> shared accumulator -> present
- [x] 20ms coalesce timer in schedulePresent; exactly one present per host per tick
- [x] Removed all "always damage" hacks and unconditional push_damage workarounds

### Multi-Client Architecture
- [x] XProtoServer persistent across sessions (owns WindowTable, PixmapTable, FontTable, etc.)
- [x] XClient per-connection state (XProtoTransport, ReplyWriter, fd, rid_base/rid_mask)
- [x] XProtoDaemon poll-based listener, creates XClient per connection
- [x] Per-client resource ownership + cleanup on disconnect (eraseOwnedBy)
- [x] Concurrent clients work (xterm + xeyes simultaneously)

### C Layer Elimination (v1.0.0)
- [x] Deleted x11_shim.c (926 lines), x11_backend.c (747 lines), x11_requests.c (~380 lines), x11_xproto.c (538 lines)
- [x] Deleted 8 associated headers (x11_shim.h, x11_backend.h, x11_requests.h, etc.)
- [x] Created SwiftBridge.cpp (extern "C" pass-throughs to C++ bridge functions)
- [x] Created X11Setup.cpp/hpp (setup handshake moved from x11_xproto.c)
- [x] UICommandQueue::push() calls x11_ui_push_*() directly (no C request queue)
- [x] Architecture simplified to: Swift <-> SwiftBridge.cpp (extern "C") <-> C++ classes

### Window Borders (v1.3.0)
- [x] Server-drawn child window borders (fillWindowBorder renders border strips in parent drawable)
- [x] border_width and border_pixel stored in WindowState/WindowView
- [x] All 5 parent-chain offset computations account for border_width (drawable at x+bw, y+bw)
- [x] Hit testing in pick functions includes border region
- [x] ChangeWindowAttributes CWBorderPixel, ConfigureWindow CWBorderWidth, GetGeometry border_width

### Window Management Enhancements
- [x] Child-to-child crossing events (v1.5.5): EnterNotify/LeaveNotify when pointer moves between children within same host
- [x] ConfigureWindow CWStackMode (v1.5.6): Above/Below/TopIf/BottomIf/Opposite sibling stacking operations
- [x] Occluded rectangle support (v1.5.6): resolveDrawableRW computes occlusion from higher-stacking siblings; ClearArea, fillWindowBackground, PolyFillRectangle skip covered pixels
- [x] PutImage resolveDrawableRW (v1.5.7): uses resolveDrawableRW for proper border_width offset and child-bounds clipping
- [x] MapSubwindows/UnmapSubwindows direct-children fix (v1.5.8): uses childrenInStackOrder() not descendantsOf()
- [x] XFILESEARCHPATH auto-setup (v1.5.8): sets env at startup so X11 clients find app-defaults

### Input / Events
- [x] Button routing fix (v1.3.0): updateMotion no longer corrupts buttons field; drag logic resilient; button handler uses HostCmd coords
- [x] Button routing: picks deepest mapped child, checks passive grabs (GrabButton), sets drag_xid
- [x] Motion routing: drag_xid during active drags, pick_motion_target otherwise
- [x] Focus delivery: sendFocusEventDirect bypasses FocusChangeMask (emulates WM SetInputFocus)
- [x] Option+click -> button 2 (middle mouse) for Xaw scrollbar thumb drag
- [x] toX11State() maps internal modifier/button bits to X11 wire positions everywhere
- [x] Key event modifier mapping fixed (sendKeyEvent uses toX11State)
- [x] GrabPointer reply re-enabled (fixes XCB sequence crash during scrollbar use)
- [x] Focus guard: stale FocusOut from destroyed window no longer steals focus

### Drawing Operations
- [x] All shape primitives: PolyPoint, PolyLine, PolySegment, PolyRectangle, PolyArc, FillPoly, PolyFillRectangle, PolyFillArc
- [x] PutImage (ZPixmap depth 24/32), CopyArea, CopyPlane, ClearArea, GetImage
- [x] Text: ImageText8/16, PolyText8/16 via BDF glyph bitmaps (16-bit added v1.4.0)
- [x] Cursor management: CreateCursor, CreateGlyphCursor, FreeCursor, cursor shape application

### Debug / Instrumentation
- [x] Two-tier trace system: #ifndef NDEBUG lifecycle traces + #ifdef X11_TRACE_VERBOSE per-op traces
- [x] Version banner: SwiftX11 v{version} at startup

### WM Compliance Hardening (v1.19.15–v1.19.24)
- [x] **`_MOTIF_WM_HINTS` support** (v1.19.15): Parse Motif decoration hints (atom 93). When flags & MWM_HINTS_DECORATIONS and decorations == 0, make window borderless+floating. Fixes JidePopup windows appearing with full title bar decorations.
- [x] **XTEST v2.2 re-enabled** (v1.19.15): Root cause of crash was GTK2/GTK3 library conflict from `LD_PRELOAD=libgdk-x11-2.0.so.0` in Docker container, not SwiftX11 bug. Fixed by removing GTK2 preload and fixing dbus-launch ordering (DISPLAY must be set before dbus-launch).
- [x] **`WM_TRANSIENT_FOR` dialog ordering** (v1.19.16–v1.19.22): Parse WM_TRANSIENT_FOR property, store parent relationship, show dialog windows above their parent via `NSApp.activate` + `makeKeyAndOrderFront` for decorated dialogs. Borderless popups use quiet `orderFront` to avoid Stage Manager disruption. `addChildWindow` approach abandoned (caused grey persistent windows on unmap).
- [x] **`acceptsFirstMouse` click-through** (v1.19.23–v1.19.24): Override on X11MTKView (the actual hit target) so clicks pass through to X11 content immediately. X11 has no "click to focus first" concept — all clicks are delivered.
- [x] **SO_SNDBUF 4MB** (v1.19.19): Increase TCP send buffer to reduce backpressure events during large PutImage bursts over Docker bridge network.
- [x] **Borderless popup focus handling** (v1.19.20): Windows made borderless via `_MOTIF_WM_HINTS` use `orderFront` instead of `makeKeyAndOrderFront` (borderless NSWindows can't become key). Prevents `canBecomeKeyWindow` warning.
- [x] **Pointer coordinate offset after edge resize** (v1.19.25): Sync window position in `windowDidResize` before C++ resize path runs. Left/top edge resize changes both origin and size; without position sync, ConfigureNotify had stale x/y.
- [x] **Stderr log timestamps** (v1.19.25): C++ `TS_FPRINTF` macro using `mach_absolute_time()` for consistent timestamps on wire trace, backpressure, error, and event-drop messages. Removed 24 unconditional Swift `print()` debug statements.
- [x] **Log window Find/Search** (v1.19.25): Cmd+F opens NSTextView find bar with incremental search. Find button in toolbar also works via `performFindPanelAction` with proper `NSMenuItem` tag.

---

## Phase 1: Core Protocol Gaps (Required for Vivado/Vitis)

Vivado uses Java Swing (renders client-side via Java 2D, uploads via PutImage). Vitis uses Eclipse SWT backed by GTK/GDK/Cairo (renders client-side, uploads via CopyArea/PutImage). Both need solid window management, events, properties, and selections.

### 1.1 GC Semantics (HIGH — affects all drawing)
GC function, planemask, and clipping are used by every toolkit.
- [x] **GC function (GXcopy, GXxor, etc.)**: Applied in all draw paths via `x11_apply_rop_argb()` with fast-path for GXcopy+full-planemask (v1.4.0). Ops updated: FillPoly, PolyFillArc, PolyArc, PolyText8/16, PutImage, CopyPlane.
- [x] **GC planemask**: Applied in all draw paths alongside GC function (v1.4.0).
- [x] **SetClipRectangles (opcode 59)**: GC clip region stored as vector of ClipRect in GCState. Parsed from wire format with clip-x-origin, clip-y-origin.
- [x] **SetDashes (opcode 58)**: Dash offset and dash list stored in GCState (v1.2.0). Rendering with dashes not yet implemented.
- [x] **GC clip enforcement**: All draw ops check GC clip rects (PolyFillRectangle uses rect-intersection, per-pixel ops use gcPointVisible). CreateGC/ChangeGC handle clip bits 17-19, CopyGC copies clip state.
- [x] **GC value mask bits 4-13, 20-22**: All 23 GC value mask bits now parsed from wire and stored in GCState (v1.2.0): line_width, line_style, cap_style, join_style, fill_style, fill_rule, tile, stipple, ts_x/y_origin, dash_offset, dashes_single, arc_mode. CopyGC copies all fields.
- [x] **GC fill-style rendering**: FillSolid/FillTiled/FillStippled/FillOpaqueStippled implemented in PolyFillRectangle, FillPoly, PolyFillArc via FillStyle.hpp helper (v1.5.0).
- [x] **GC tile/stipple rendering**: Tile pixmap sampled at (px-ts_x_origin)%tile_w, stipple bit-tested at (px-ts_x_origin)%stip_w with proper modulo wrapping (v1.5.0).

### 1.2 Missing Opcodes (HIGH — crash prevention)
Unhandled opcodes log warnings but don't send replies, causing XCB sequence desync for reply-bearing requests. Java/GTK may use any of these.
- [x] **ReparentWindow (opcode 7)**: Updates parent chain in WindowTable, sends ReparentNotify event, handles unmap/remap (v1.2.0).
- [x] **ChangeActivePointerGrab (opcode 30)**: Updates active grab event mask via GrabTable (v1.2.0).
- [x] **QueryKeymap (opcode 44)**: Returns 32 zero bytes (no keys pressed) (v1.2.0).
- [x] **GetMotionEvents (opcode 39)**: Returns empty event list (v1.2.0).
- [x] **SetFontPath (opcode 51)**: Accepts and ignores (v1.2.0).
- [x] **GetFontPath (opcode 52)**: Returns empty font path list (v1.2.0).
- [x] **DestroySubwindows (opcode 5)**: Destroys all descendants in depth-first order (v1.2.0).
- [x] **RotateProperties (opcode 114)**: Accepts and ignores (stub) (v1.2.0).

### 1.3 16-bit Text (MEDIUM — Unicode support)
Java/GTK use 16-bit text for internationalized strings.
- [x] **PolyText16 (opcode 75)**: 16-bit character strings via CHAR2B encoding, supports font changes (v1.4.0).
- [x] **ImageText16 (opcode 77)**: 16-bit text with opaque background fill (v1.4.0).
- [x] **QueryTextExtents**: Already handles 16-bit CHAR2B characters correctly (verified v1.5.0).

### 1.4 Selections / Clipboard (MEDIUM — copy/paste)
Vivado/Vitis need clipboard for copy/paste between X11 apps and potentially with macOS.
- [x] **Selection request/notify flow**: ConvertSelection/SelectionNotify works end-to-end. macOS pasteboard served when no X11 owner (v1.5.0).
- [x] **CLIPBOARD atom**: Pre-registered as atom 69 along with TARGETS(70), UTF8_STRING(71), TIMESTAMP(72), TEXT(73), MULTIPLE(74), INCR(75), WM_PROTOCOLS(76), WM_DELETE_WINDOW(77) (v1.5.0).
- [x] **TARGETS**: ConvertSelection for TARGETS returns list of supported types (UTF8_STRING, STRING, TEXT, TIMESTAMP) (v1.5.0).
- [x] **macOS clipboard bridge**: Swift registers get/set callbacks via x11_clipboard_register(). C++ reads NSPasteboard for CLIPBOARD/PRIMARY ConvertSelection when no X11 owner. X11→macOS sync intercepts SelectionNotify in SendEvent (v1.5.0).
- [x] **Bidirectional clipboard sync** (v1.5.1): X11→macOS: ClipboardCapture detects property writes with UTF8_STRING/STRING data, pushes to NSPasteboard. Root-proxy selection owner (XID 1) assigned after capture so subsequent ConvertSelection from other X11 clients routes through server (reads cached macOS clipboard). macOS→X11: Option-click paste in xterm triggers ConvertSelection → server serves from NSPasteboard. Sequence number stamping fix in handleSendEvent ensures correct event delivery.

### 1.5 WarpPointer (MEDIUM — stub upgrade)
- [x] **WarpPointer (opcode 41)**: Implemented via UICommandQueue → Swift → CGWarpMouseCursorPosition (v1.4.0). Supports root-relative, window-relative, and delta warps.

---

## Phase 2: X11 Extensions (Required for Modern Toolkits)

Java 2D, GTK/Cairo, and Pango all query for extensions. **Current state (v1.9.7)**: **BIG-REQUESTS**, **RENDER**, **XFIXES**, **RANDR**, **XINERAMA**, **Generic Event Extension (GE)**, and **SHAPE** are all advertised as present (7 total). All RENDER operations fully implemented including Trapezoids, Triangles, gradient sources. RANDR dynamically reports real multi-monitor configuration via ScreenLayout cache (v1.9.7). Xinerama reports real per-monitor screens. SHAPE fully implemented with actual pixel-level clipping, transparent backgrounds, and hit testing.

### 2.1 RENDER Extension (HIGH — anti-aliased fonts, alpha compositing) — ADVERTISED (v1.6.0)
The single most impactful extension. Java 2D's XRender pipeline, GTK/Cairo's rendering, and Pango's font rendering all use RENDER. Without it, clients fall back to core protocol (bitmap fonts, no alpha blending).
- [x] **QueryExtension("RENDER")**: Advertised as present=1, major opcode 139 (v1.6.0).
- [x] **RenderQueryVersion**: Returns version 0.11 (v1.4.0).
- [x] **RenderQueryPictFormats**: Returns ARGB32, RGB24, A8, A4, A1 formats + screen mapping to TrueColor visual (v1.4.0).
- [x] **CreatePicture / FreePicture**: Picture table maps PID → drawable+format (v1.4.0).
- [x] **Composite**: All 12 Porter-Duff blend modes: Src, Over, Add, Clear, OverReverse, In, InReverse, Out, OutReverse, Atop, AtopReverse, Xor (v1.6.0). Solid-fill, drawable-to-drawable, and mask paths.
- [x] **Mask parameter in Composite**: Solid masks (uniform alpha multiplier) and drawable masks (per-pixel alpha modulation) (v1.6.0).
- [x] **FillRectangles**: Solid color fill with compositing ops (v1.4.0).
- [x] **CreateSolidFill**: Solid color source pictures (v1.4.0).
- [x] **ChangePicture**: Parses CPRepeat attribute (v1.4.0).
- [x] **QueryFilters**: Returns "nearest" and "bilinear" filter names (v1.4.0).
- [x] **CreateGlyphSet / FreeGlyphSet / ReferenceGlyphSet**: GlyphSet table with format tracking (v1.6.0).
- [x] **AddGlyphs**: Full glyph storage — parses wire format (12B glyph info + bitmap data), converts A1/A4/A8 to internal A8, stores in per-glyphset map (v1.6.0).
- [x] **FreeGlyphs**: Removes glyphs from glyphset (v1.6.0).
- [x] **CompositeGlyphs8/16/32**: Full rendering — parses GlyphElt wire format (len/dx/dy/glyph IDs, len=255 glyphset switch). maskFormat path accumulates into temp A8 buffer for single Composite pass; no-mask path composites each glyph directly. Source color modulation + compositing op applied (v1.6.0).
- [x] **SetPictureClipRectangles / SetPictureTransform / SetPictureFilter**: Consume silently (v1.4.0).
- [x] **Trapezoids (minor 10)**: Scanline rasterization of TRAPEZOID structs (40B each: top/bottom FIXED 16.16, left/right LINEFIX). Interpolates left/right x edges per scanline, fills with source color via compositing op (v1.6.0).
- [x] **Triangles / TriStrip / TriFan (minor 11/12/13)**: Scanline triangle rasterization. Triangles: independent triangles (3 POINTFIXes each). TriStrip: each new point adds triangle with previous two. TriFan: all triangles share first point. Sort vertices by Y, walk edges with FIXED 16.16 interpolation (v1.7.1).
- [x] **Gradient fills (CreateLinearGradient/Radial/Conical)**: Full gradient source pictures with per-pixel sampling in Composite. Linear: dot-product projection onto gradient axis. Radial: concentric circle distance. Conical: atan2 angle. Color stop interpolation with Pad (clamp) mode. Single-stop gradients optimized to solid fills (v1.7.1).

### 2.2 BIG-REQUESTS Extension (HIGH — large images)
Vivado schematics and waveform views can be large. Without BIG-REQUESTS, maximum request size is 262140 bytes (~256KB), limiting PutImage to ~256KB per call.
- [x] **QueryExtension("BIG-REQUESTS")**: Returns present=1, major opcode 133 (v1.4.0).
- [x] **BigReqEnable**: Returns max request length 1M words (4MB). Sets per-client big_req_enabled flag (v1.4.0).
- [x] **Wire format**: readAndDispatch() detects len_words==0 when big_req_enabled, reads 4-byte extended length (v1.4.0).

### 2.3 XFIXES Extension (MEDIUM — cursor, regions) — ADVERTISED (v1.6.0)
GTK and Java use XFIXES for cursor visibility and region operations.
- [x] **QueryExtension("XFIXES")**: Advertised as present=1, major opcode 134 (v1.6.0).
- [x] **XFixesQueryVersion**: Returns version 5.0 (v1.4.0).
- [x] **XFixesShowCursor / HideCursor**: Consume silently (v1.6.0).
- [x] **XFixesSelectCursorInput**: Consume silently — cursor event selection (v1.6.0).
- [x] **XFixesGetCursorImage**: Returns 1x1 transparent cursor reply (v1.6.0).
- [x] **XFixesCreateRegion / DestroyRegion**: Track XID existence silently. Also CreateRegionFromBitmap/Window/GC/Picture (v1.6.0).
- [x] **XFixesSetWindowShapeRegion**: Consume silently (v1.6.0).
- [x] **XFixesGetCursorImageAndName**: Returns 1x1 transparent cursor + empty name (v1.6.0).

### 2.4 SHAPE Extension (non-rectangular windows) — DONE, ADVERTISED (v1.7.5)
Full SHAPE implementation with actual pixel-level clipping, transparent backgrounds, and hit testing. xeyes displays with transparent eye-shaped windows.
- [x] **QueryExtension("SHAPE")**: Advertised as present=1, major opcode 135 (v1.7.5).
- [x] **ShapeQueryVersion**: Returns version 1.1 (v1.4.0).
- [x] **ShapeRectangles (sub-opcode 1)**: Parses wire format (op, kind, window, x/y offset, rects), stores in ShapeRegion per window (v1.7.5).
- [x] **ShapeMask (sub-opcode 2)**: Reads depth-1 pixmap from PixmapTable, converts bitmap to rectangles via run-length encoding (v1.7.5).
- [x] **ShapeCombine (sub-opcode 3)**: Combines source window shape into dest window shape with offset (v1.7.5).
- [x] **ShapeOffset (sub-opcode 4)**: Offsets shape region by dx/dy (v1.7.5).
- [x] **ShapeQueryExtents (sub-opcode 5)**: Returns actual shaped booleans and extents from stored ShapeRegion (v1.7.5).
- [x] **ShapeGetRectangles (sub-opcode 8)**: Returns actual rectangle list from stored ShapeRegion (v1.7.5).
- [x] **ShapeRegion data structure**: Stores bounding/clip/input regions per window. Supports Set/Union/Intersect/Subtract/Invert operations. setFromBitmap scans rows with LSBFirst bit order, run-length encodes set bits (v1.7.5).
- [x] **Depth-1 pixmap drawing**: PolyFillArc and PolyFillRectangle now handle depth-1 pixmap targets (bit manipulation with LSBFirst order). Required because xeyes creates elliptical masks via XFillArc on depth-1 bitmaps (v1.7.5).
- [x] **Hit testing**: InputRouting.cpp and XProtoNotifyBridge.cpp check shape containment after rectangular bounds check. Input shape checked first, falls back to bounding shape (v1.7.5).
- [x] **Visual clipping**: Present-time alpha masking in Swift — premultiplied alpha (transparent=0x00000000), shape rects queried via C++ bridge. NSWindow/CAMetalLayer/SwiftUI layers all set non-opaque. Metal pipeline uses alpha blending (v1.7.5).
- [x] **Shaped window resize**: Retained display buffer skipped for shaped windows, new surfaces filled with transparent black (v1.7.5).

### 2.5 Other Extensions (LOW — query but don't need full impl)
These are frequently queried. Return present=0 with correct reply format, or minimal stubs:
- [x] **MIT-SHM**: Not applicable over network/container — returns present=0 (intentional, v1.4.0).
- [x] **RANDR**: Advertised (v1.7.4). RRQueryVersion returns 1.3. Dynamic multi-monitor (v1.9.7): ScreenLayout cache queries CGGetActiveDisplayList, reports real per-monitor outputs/CRTCs/modes. Handlers: GetScreenResources/Current, GetOutputInfo (36-byte reply), GetCrtcInfo, GetCrtcTransform (identity), GetPanning/SetPanning, GetScreenSizeRange, GetOutputPrimary, GetProviders, ListOutputProperties, SelectInput, SetCrtcConfig, GetCrtcGammaSize. `xrandr --query` works.
- [x] **Xinerama**: Advertised (v1.7.4). QueryVersion returns 1.1, IsActive=1. Dynamic multi-monitor (v1.9.7): QueryScreens returns real per-monitor entries from ScreenLayout.
- [x] **DPMS**: Display power management — returns present=0 (intentional, not applicable on macOS, v1.4.0).
- [x] **Generic Event Extension (GE)**: Advertised (v1.7.4). GEQueryVersion returns 1.0.

---

## Phase 3: Font Infrastructure (Required for Readable UI)

~~Vivado and Vitis need proper fonts. The current BDF-only system works for xterm but won't satisfy GTK/Java apps that expect a richer font ecosystem.~~

### 3.1 Font Matching and XLFD — DONE (v1.7.0)
- [x] **Wildcard XLFD matching**: Iterative glob matching with `*` and `?` support. Matches against both builtin BDF fonts and PCF registry XLFD names.
- [x] **Font aliases**: System `fonts.alias` files loaded from `/opt/X11/share/fonts/{misc,75dpi,100dpi}/`. Maps aliases like "fixed" → specific XLFD.
- [x] **Scaled fonts**: Closest-match by pixel size from available BDF fonts (picks closest bbx_h). PCF fonts provide native sizes for many standard sizes.

### 3.2 Additional Fonts — DONE (v1.7.0)
- [x] **PCF font support**: Full PCF parser with TOC, metrics, bitmaps, encoding tables. Handles both MSB-first and LSB-first bit order (byte-level bit reversal for LSB). zlib decompression for .pcf.gz files.
- [x] **Font directory scanning**: Scans `/opt/X11/share/fonts/{misc,75dpi,100dpi}/` at startup. Parses `fonts.dir` files for XLFD→filename mapping. Lazy-loads PCF fonts on first use.
- [x] **Symbol font encoding**: Adobe Symbol font (`adobe-fontspecific` encoding) works correctly — xcalc √, ÷, π characters render from symb12.pcf.gz.
- [x] **ListFontsWithInfo** (opcode 50): Full implementation returns per-font metrics + name, followed by correct 60-byte terminator reply.

### 3.3 TrueType / CoreText Integration ✅ (v1.8.0)
Client-side font rendering via RENDER CompositeGlyphs (Pango/FreeType) is the primary path for modern toolkits. Server-side PCF/BDF fonts cover legacy apps (xterm, xcalc, xclock).
- [x] **Xft/fontconfig on client side**: RENDER CompositeGlyphs8/16/32 fully implemented (v1.6.0+). Clients use FreeType + fontconfig → AddGlyphs → CompositeGlyphs.
- [x] **CoreText bridge**: Maps X11 font requests (XLFD or bare names) to macOS system fonts via CoreText C API. Rasterizes Latin-1 glyphs with both 1-bit bitmap and 8-bit alpha coverage. Runtime toggle for antialiased vs crisp rendering. Settings menu in Preferences → Rendering. Family mapping: fixed→Menlo, courier→Courier, helvetica→Helvetica, times→Times New Roman, etc. CoreText fonts tried first in FontTable lookup chain, PCF/BDF fallback for cursor/symbol/fontspecific fonts.

---

## Phase 4: Robustness and Correctness

### 4.1 Error Handling (HIGH) — DONE (v1.9.3)
- [x] **Proper X11 error generation**: BadWindow, BadDrawable, BadPixmap, BadFont, BadAtom, BadColor, BadValue across ~50 request handlers in 14 files. Three tiers: reply-bearing (XCB desync prevention), void resource-modifying (spec compliance), drawing ops. Uses `ctx.transport().sendErrorCore()` infrastructure.
- [x] **Error reply format**: 32-byte error replies with correct error code, seq, resourceId, minor opcode, major opcode via `buildCoreError32()` in WireErrors.hpp.
- [x] **Error for destroyed resources**: Drawing ops check drawable existence before sending BadDrawable — valid-but-unresolvable drawables (depth-1 pixmaps, unmapped windows) silently skip; truly non-existent XIDs get proper errors.

### 4.2 Wire Protocol Correctness (MEDIUM)
- [x] **Big-endian clients**: Rejected at handshake with clear error. ByteReader has BE methods ready if needed. All practical targets (x86/ARM Docker for Vivado) are LE. Full BE support deferred.
- [x] **Padding verification**: All replies use `sendPaddedBytes`/`sendReplyWithPaddedPayload`/`pad4_u32()` for correct 4-byte alignment. 32-byte base replies inherently aligned.
- [x] **Request length validation**: Dispatch loop auto-skips unconsumed bytes after handler (XProtoServer.cpp). Per-handler `br.remaining()` minimum checks. Reply safety net sends `BadImplementation` for missing replies.

### 4.3 Window Management Correctness (MEDIUM)
- [x] **ConfigureWindow stack mode**: Above/Below/TopIf/BottomIf/Opposite sibling stacking with CWSibling support (v1.5.6).
- [x] **ConfigureWindow border width**: Tracked, stored, and rendered (v1.3.0).
- [x] **Override-redirect**: Parsed in CreateWindow/ChangeWindowAttributes (bit 9), stored in WindowState, wired through UICommandQueue to Swift. Creates borderless NSWindow with floating level. Override-redirect windows don't steal focus on map. GetWindowAttributes/ConfigureNotify/ReparentNotify return actual stored value (v1.9.5).
- [x] **Gravity**: win_gravity (bit 5) and bit_gravity (bit 4) parsed, stored, and reported correctly. Actual gravity-based position adjustment during resize deferred (low priority — most X11 toolkits handle gravity client-side) (v1.9.5).
- [x] **Backing store**: Parsed in CreateWindow/ChangeWindowAttributes (bit 6), stored, reported in GetWindowAttributes. Stored as stub (NotUseful behavior — server never saves/restores obscured pixels) (v1.9.5).

### 4.4 Colormap (LOW — TrueColor is sufficient)
SwiftX11 advertises TrueColor visual. Vivado/Vitis Java apps use TrueColor. Current colormap stubs (accept requests, return reasonable defaults) should work.
- [ ] **Verify TrueColor visual advertisement**: Ensure depth=24, class=TrueColor, red/green/blue masks correct in setup reply.
- [ ] **AllocColor correctness**: For TrueColor, AllocColor should return the closest matching pixel value (currently returns the RGB packed into pixel — verify this is correct).

---

## Phase 5: Performance and Polish

### 5.1 Rendering Performance (MEDIUM) — DONE (v1.10.0)
- [x] **Software present path**: Persistent backing buffer with partial-row copy (only damaged rows memcpy'd). Avoids full-surface copy on every frame. 3-frame full-copy countdown on resize (matches Metal's fullUploadCountdown). (v1.10.0)
- [x] **PutImage optimization**: GXcopy fast path uses bulk memcpy + 4-pixel-unrolled alpha forcing instead of per-pixel copy-and-OR. (v1.10.0)
- [x] **Expose coalescing**: sendExposeSubtree and ExposeChildren now set the X11 Expose `count` field correctly (count=N-1 for first event, decrementing to 0 for last), allowing clients to defer redrawing until the final Expose. (v1.10.0)

### 5.2 Container / Network Support (HIGH for Vivado use case) — DONE (v1.9.4)
- [x] **TCP socket listener**: TCP now binds to 0.0.0.0 (all interfaces) by default. Docker containers connect via `DISPLAY=host.docker.internal:1`. (v1.9.4)
- [x] **Unix socket**: /tmp/.X11-unix/X{display} Unix domain socket listener for local containers via volume mount (`-v /tmp/.X11-unix:/tmp/.X11-unix`). (v1.9.4)
- [x] **Multi-listener architecture**: XProtoDaemon supports simultaneous TCP + Unix listeners with vector-based poll loop. Settings UI toggles for TCP/Unix. (v1.9.4)
- [x] **Xauth / Latency tolerance**: Moved to Phase 7 (LOW) — not needed for local/LAN Vivado use case.

### 5.3 Keyboard (MEDIUM) — DONE (v1.11.0)
- [x] **Full keysym mapping**: Comprehensive macOS virtual keycode → X11 keysym table covering all keys: letters (US layout), digits, punctuation, F1-F20, Home/End/PageUp/PageDown/Delete/Help, arrows, keypad (KP_0-KP_9, operators, KP_Enter, KP_Equal), modifiers (left+right for Shift/Ctrl/Alt/Super), CapsLock, Fn→Meta_L, ISO Section key. All keysym constants added to KeySyms.hpp (v1.11.0).
- [x] **Modifier mapping**: GetModifierMapping returns 2 keys per modifier (left+right): Shift_L/R, CapsLock, Control_L/R, Alt_L/R (Mod1), Command_L/R (Mod4). `xmodmap -pm` shows correct mapping (v1.11.0).
- [x] **Keymap state**: QueryKeymap returns real pressed-key state via InputState::keymap_ (256-bit bitfield). Key handler updates keymap on press/release (v1.11.0).
- [x] **GetKeyboardMapping 4-column**: Reports 4 keysyms per keycode (normal, shift, mode_switch, mode_switch+shift) as expected by Java Swing/GTK. Columns 3-4 are NoSymbol on macOS (no Mode_switch/AltGr) (v1.11.0).
- [x] **XKB**: Moved to Phase 7 (LOW) — Java Swing/GTK fall back to core keyboard gracefully. Not needed for Vivado.

### 5.4 Window Close / Client Lifecycle (HIGH — user experience) — DONE (v1.9.0)
- [x] **Window close kills client**: Red close button and Cmd+W now terminate X11 clients. ICCCM-compliant: checks WM_PROTOCOLS for WM_DELETE_WINDOW, sends ClientMessage if supported. Falls back to forceful socket disconnect for clients that don't support WM_DELETE_WINDOW (v1.9.0).
- [x] **Keyboard shortcut**: Cmd+W triggers `performClose` on the key NSWindow, which fires `windowWillClose` → `x11_post_window_close`. Works automatically because X11 windows have `.closable` style (v1.9.0).

### 5.5 Window Menu Integration (MEDIUM — user experience) — DONE (v1.11.1)
- [x] **Window menu entries**: macOS Window menu via `NSApp.windowsMenu` — AppKit automatically lists all X11 NSWindows with correct titles and brings them to front on selection (v1.11.1).
- [x] **WM_NAME title sync**: PropOps hooks WM_NAME (atom 39) and `_NET_WM_NAME` (atom 79) property changes → `x11_ui_push_title()` → NSWindow title update. Child window titles route through `topLevelAncestorOf()` (v1.11.1).
- [x] **Dynamic updates**: AppKit handles add/remove automatically when NSWindows are created/destroyed (v1.11.1).

### 5.6 SwiftX11 Debug Panel Redesign (LOW — UX improvement) — DONE (v1.13.0)
Convert the SwiftX11 main window from a spawnable `WindowGroup` into a single persistent debug/control panel.
- [x] **Single persistent window**: `Window("SwiftX11 Log", id: "log-window")` replaces `WindowGroup`. No "New SwiftX11 Window" in File menu.
- [x] **View menu show/hide**: "Toggle Log Window" (⌘0) in View menu shows/hides the log window without spawning new instances.
- [x] **Debug logging controls**: Log verbosity picker (Errors only / Info / Verbose) in ContentView, wired to C++ via `x11_set_log_verbosity()`.
- [x] **Log output display**: Monospace `LogTextView` with auto-scroll, Copy All, Clear buttons. Trace category filtering deferred (optional).
- [x] **Remove obsolete features**: No obsolete controls remain — clean UI with only essential logging controls.

### 5.7 ICCCM / Window Manager Compliance (MEDIUM) — DONE (v1.12.0)
- [x] **Minimum window size floor**: Top-level windows created below 200×100 are enlarged in C++ CreateWindow before WindowTable upsert. Swift NSWindow applies matching floor. Fixes Vivado 1×1 splash screen (v1.12.0).
- [x] **WM_NORMAL_HINTS**: Full ICCCM XSizeHints parser — PSize, PMinSize, PMaxSize, PResizeInc, PBaseSize. Tiny windows auto-resized. Min/max/increment pushed to Swift → NSWindow.contentMinSize/contentMaxSize/contentResizeIncrements (v1.12.0).
- [x] **WM_HINTS**: Parses InputHint (wants_input stored in WindowTable) and StateHint (IconicState → miniaturize on map). Icon hints skipped (v1.12.0).
- [x] **WM_PROTOCOLS**: WM_TAKE_FOCUS implemented — Focus handler sends ClientMessage(WM_PROTOCOLS, WM_TAKE_FOCUS) before FocusIn when client advertises it. wants_take_focus cached in WindowTable for fast lookup. WM_DELETE_WINDOW already implemented (v1.9.0) (v1.12.0).
- [x] **_NET_WM_* (EWMH)**: Pre-registered atoms 81-92. _NET_WM_WINDOW_TYPE → NSWindow style mapping (NORMAL/DIALOG/TOOLBAR/UTILITY/MENU/TOOLTIP/SPLASH). _NET_WM_STATE MODAL → .modalPanel level, FULLSCREEN → toggleFullScreen. _NET_FRAME_EXTENTS set proactively on MapWindow (left=0, right=0, top=28 for titled, bottom=0) (v1.12.0).

---

## Phase 6: UX Polish & Help (MEDIUM — general release readiness)

### 6.1 Help Menu / User Guide ✅ (v1.14.0)
- [x] **Help menu item**: "SwiftX11 Help" (⌘⇧/) opens HelpView.swift in a native NSWindow.
- [x] **DISPLAY configuration**: TCP and Unix socket setup, `~/.profile` instructions.
- [x] **Font locations**: System font paths, CoreText bridge, antialiased font toggle.
- [x] **Settings documentation**: Rendering and Network panels explained.
- [x] **Log window**: What it shows, View menu toggle.
- [x] **Docker/container workflow**: `DISPLAY=host.docker.internal:1`, TCP vs Unix socket on Docker Desktop.
- [x] **Keyboard & Mouse**: Option+click → middle button, Ctrl+click → right-click, Cmd+W → close.
- [x] **Copy & Paste**: X11 selection protocol explained for macOS users.
- [x] **Known limitations**: Metal required, no GLX, no XKB compose, font charset gaps.

### 6.2 About Dialog ✅ (v1.14.0)
- [x] **About SwiftX11**: Version, build date (`__DATE__`), credits (Lawrence Gibbons + Claude). Uses `NSApp.orderFrontStandardAboutPanel(options:)` via SwiftUI CommandGroup.

---

## Phase 7: Additional Extensions (LOW — broader app compatibility)

These extensions are not needed for the Vivado/Vitis target but may be required by other X11 applications colleagues might use.

### 7.0 Deferred Items (LOW)
Items moved from higher phases — not blocking Vivado/Vitis.
- [ ] **Xauth**: Basic MIT-MAGIC-COOKIE-1 authentication (or xhost + for development). Only needed if exposing server to untrusted networks.
- [ ] **Latency tolerance**: Audit protocol handling for WAN latency assumptions. Not relevant for local/LAN use.
- [ ] **XKB extension**: Full X Keyboard Extension for multi-layout support, compose/dead keys, per-key type definitions. Java Swing/GTK fall back to core keyboard protocol gracefully. Return not-present is acceptable.
- [ ] **Option key compose**: Pass macOS Option-key-interpreted characters (e.g., Option+e → é) through to X11 keysym table columns 3-4.

### 7.1 XC-MISC Extension (HIGH — prevents XID exhaustion crash) ✅ Done (v1.14.1)
Long-running clients (Vivado sessions that run for hours/days) exhaust their 2^21 XID range and need `XC-MiscGetXIDRange` to get more. Without it, the client crashes when XIDs run out. Very simple extension (3 minor opcodes).
- [x] **XC-MiscGetVersion** (minor 0): Returns version 1.1. Advertised in QueryExtension + ListExtensions (major opcode 140, total 8 extensions).
- [x] **XC-MiscGetXIDRange** (minor 1): Returns new XID range (start + count) from per-client allocator. Allocates from midpoint of rid_mask upward to avoid collision with Xlib's bottom-up allocation.
- [x] **XC-MiscGetXIDList** (minor 2): Returns list of individual free XIDs (capped at 4096 per request). Per-client cursor in XClient tracks allocation state.

### 7.2 XInput / XInput2 (MEDIUM-HIGH — GTK3/4 apps) ✅ Done (v1.15.4)
GTK3/4 queries XI2 at startup. Full XI2 event delivery infrastructure with 6 event senders and global RawMotion support. xeyes uses XI2 RawMotion for pupil tracking; works correctly with shaped windows.
- [x] **XIQueryVersion** (minor 47): Returns version 2.0. Advertised as "XInputExtension" in QueryExtension + ListExtensions (major opcode 141).
- [x] **XIQueryDevice** (minor 48): Returns 4 virtual core devices (pointer + keyboard masters, XTEST pointer + keyboard slaves) with ButtonClass and KeyClass entries.
- [x] **XISelectEvents** (minor 46): Per-window XI2 mask storage + root window mask in InputState. Supports XI_RawMotion and all standard XI2 event types.
- [x] **XIQueryPointer** (minor 40): Returns pointer position and modifiers for XI2 device.
- [x] **XIGetClientPointer** (minor 44): Returns virtual core pointer device ID.
- [x] **XI2 events** (v1.15.3): 6 XI2 event senders (Motion, Button, Key, Crossing, Focus, RawMotion) with 13 injection points alongside core events. RawMotion uses 68-byte variable-length GenericEvent with 2-axis valuator data. `first_event=93` avoids overwriting core event handlers in libXi.
- [x] **Global RawMotion** (v1.15.3): `GlobalPointerTracker` (NSEvent global+local monitor + 30fps timer) delivers RawMotion to all clients with xi2_root_mask regardless of cursor position over X11 windows. Required for xeyes pupil tracking.

### 7.3 XTEST Extension (MEDIUM — automation/accessibility) ✅ Done (v1.14.2)
Synthesizes keyboard/mouse events. Used by accessibility tools (AT-SPI), automation (xdotool, xte), and test frameworks. GTK accessibility stack queries for it.
- [x] **XTestGetVersion** (minor 0): Returns version 2.2. Advertised in QueryExtension + ListExtensions (major opcode 142). Total advertised extensions: 10.
- [x] **XTestFakeInput** (minor 2): Synthesizes KeyPress/KeyRelease, ButtonPress/ButtonRelease, MotionNotify via `x11_post_pointer_move2` / `x11_post_button` / `x11_post_key` bridge functions.
- [x] **XTestGrabControl** (minor 3): Silently consumes grab control requests.

### 7.4 SYNC Extension (LOW)
Synchronization primitives. Some compositors and toolkits use SYNC for frame synchronization.
- [ ] **SYNC QueryVersion**: Return present=0 initially, implement if needed.
- [ ] **SYNC counters/fences**: Full implementation if required by specific apps.

### 7.5 DAMAGE Extension (LOW — compositor awareness)
Tracks window content changes. Some GTK apps query for it but fall back gracefully. SwiftX11 already tracks damage internally; this would expose it to clients.
- [ ] **DamageQueryVersion**: Return version or present=0. Assess whether any target apps require it.
- [ ] **DamageCreate/Destroy**: Track damage objects per drawable.
- [ ] **DamageNotify events**: Send DamageNotify events to subscribed clients when drawable content changes.

### 7.6 DBE — Double Buffer Extension (LOW — flicker-free legacy apps)
Some older Xaw/Motif apps (and `xclock -render`) use DBE for flicker-free rendering. Front/back buffer swap.
- [ ] **DBE QueryVersion**: Return version or present=0.
- [ ] **DBEAllocateBackBuffer / DeallocateBackBuffer**: Create/destroy back buffer for a window.
- [ ] **DBESwapBuffers**: Copy back buffer to front buffer and present.

### 7.7 Window Shape AA Compositing — DONE (v1.15.5)
Antialiased window shape borders at the macOS compositing level. 3×3 box-filter coverage AA replaces binary masking — boundary pixels get fractional alpha proportional to neighbor coverage, producing smooth window outlines.
- [x] **Shape mask AA**: 3×3 box-filter coverage grid in `applyShapeMask()` — interior pixels fully opaque (fast-path via cardinal-neighbor check), exterior fully transparent, boundary pixels get `alpha = coverage/9 * 255`. Uses **straight alpha** (RGB preserved as-is, only alpha channel set).
- [x] **Metal blending**: Pipeline uses `sourceAlpha/oneMinusSourceAlpha` (straight alpha, not premultiplied) — Metal handles the alpha blending at render time. Previous premultiplied attempt (v1.15.0) reverted due to double-application artifacts.

---

## Testing Applications by Phase

Recommended X11 apps for validating each phase. Install via Homebrew (`brew install --cask xquartz` for base X11 tools) or from a Linux container. Most are available in the `x11-apps`, `xterm`, and `x11perf` packages.

### Already Working (Baseline)
| App | What it tests | Install |
|-----|---------------|---------|
| **xterm** (`xterm -sb -rightbar -bc`) | Core drawing (PutImage, text), input (keyboard, mouse), scrollbar (grabs, button routing), focus, cursors | `brew install xterm` or container |
| **xeyes** | Pointer tracking (QueryPointer), window shape, motion events, multi-client | `x11-apps` package |

### Phase 1: Core Protocol Gaps
| App | What it tests | Install |
|-----|---------------|---------|
| **xdpyinfo** | Display/visual/extension enumeration — reveals what the server advertises | `x11-utils` package |
| **xwininfo** | Window geometry, attributes, tree — tests QueryTree, GetWindowAttributes, GetGeometry | `x11-utils` package |
| **xprop** | Property read/write — tests GetProperty, ChangeProperty, ListProperties | `x11-utils` package |
| **xev** | Event diagnostics — shows every event type with full detail (masks, modifiers, state fields) | `x11-utils` package |
| **xclock** (`xclock -analog`) | Xaw widgets, PolyLine, PolyFillArc, timer events, GC clipping | `x11-apps` package |
| **xcalc** | Complex Xaw widget tree, ReparentWindow (if using Xt shell), keyboard input, GC operations | `x11-apps` package |
| **xedit** | Text editing widget, selection (PRIMARY), multi-line text rendering | `x11-apps` package |
| **xclipboard** | Clipboard/selection protocol end-to-end (CLIPBOARD, PRIMARY, TARGETS) | `x11-apps` package |
| **xgc** | GC function exerciser — tests all GC functions (GXxor, GXand, etc.), line styles, fill styles, dashes | `x11-apps` package |
| **xmag** | Screen magnifier — tests GetImage, CopyArea, pixmap operations | `x11-apps` package |
| **x11perf** | Drawing performance benchmarks — stresses every core drawing op at volume | `x11perf` package |

### Phase 2: X11 Extensions
| App | What it tests | Install |
|-----|---------------|---------|
| **rendercheck** | RENDER extension test suite — validates Composite, glyph ops, picture formats, blend modes | `rendercheck` package |
| **cairo-demo-*** | Cairo rendering tests — exercises RENDER through Cairo's XRender backend | Build from `cairo` source (`make check`) |
| **gtk3-demo** | GTK3 widget showcase — tests RENDER, XFIXES, font rendering, complex widget trees | `gtk+3` package (`gtk3-demo`) |
| **gtk4-demo** | GTK4 widget showcase — similar but may use different rendering paths | `gtk4` package (`gtk4-demo`) |
| **gedit** / **mousepad** | Lightweight GTK text editors — practical RENDER + font + clipboard test | Container: `apt install gedit` or `mousepad` |
| **xfce4-terminal** | GTK terminal emulator — more complex than xterm, uses RENDER for text | Container: `apt install xfce4-terminal` |

### Phase 3: Font Infrastructure
| App | What it tests | Install |
|-----|---------------|---------|
| **xfontsel** | Interactive XLFD font browser — tests ListFonts with wildcards, OpenFont, QueryFont | `x11-apps` package |
| **xlsfonts** | Lists all available fonts — tests ListFonts pattern matching | `x11-apps` package |
| **xfd** (`xfd -fn fixed`) | Displays all glyphs in a font — tests font metrics, 16-bit character rendering | `x11-apps` package |

### Phase 4: Robustness
| App | What it tests | Install |
|-----|---------------|---------|
| **xdpyinfo** (revisit) | Verify error-free extension/visual enumeration after robustness improvements | Already installed |
| **ico** | Animated 3D icosahedron — stresses rapid PolyLine + ClearArea + window management | `x11-apps` package |
| **bitmap** | Bitmap editor — tests detailed pixmap operations, XBM format | `bitmap` package |
| **twm** | Classic X11 window manager — exercises WM_PROTOCOLS, ICCCM, ConfigureWindow, ReparentWindow | Container: `apt install twm` |

### Phase 5: Performance & Container
| App | What it tests | Install |
|-----|---------------|---------|
| **x11perf** (revisit) | Benchmark before/after optimization — measures throughput for all draw ops | Already installed |
| **glxgears** / **glxinfo** | OpenGL/GLX queries (expect graceful failure or stub) | `mesa-utils` package |
| **Docker xterm** | Validates TCP socket + DISPLAY forwarding from container | `docker run -e DISPLAY=host.docker.internal:0 ...` |

### Final Validation: Vivado/Vitis
| App | What it tests | Install |
|-----|---------------|---------|
| **Vivado GUI** | Java Swing — full validation of PutImage, GC ops, fonts, window management, BIG-REQUESTS | Xilinx Vivado in Docker container |
| **Vitis IDE** | Eclipse SWT/GTK — full validation of RENDER, Cairo, Pango, complex widget trees | Xilinx Vitis in Docker container |
| **Vivado Waveform Viewer** | Large PutImage (schematics), scrolling, zoom — stress test for BIG-REQUESTS + performance | Part of Vivado |

### Quick Smoke Test Script
```bash
# Run after each phase to verify nothing regressed
export DISPLAY=127.0.0.1:1

# Baseline (should always work)
xterm -sb -rightbar -bc &
sleep 1
xeyes &
sleep 1

# Phase 1+ diagnostics
xdpyinfo | head -20          # display info
xwininfo -root                # root window
xprop -root                   # root properties
xev -event keyboard &         # event monitor

# Phase 1+ drawing
xclock -analog &
xcalc &

# Phase 2+ (RENDER — now advertised in v1.6.0)
rendercheck                 # RENDER extension tests
# gtk3-demo &               # run when GTK font infrastructure is ready
```

---

## Priority Order for Vivado/Vitis

### Completed milestones
~~1. **GC clipping (SetClipRectangles)** — DONE (v1.1.0)~~
~~2. **Missing reply-bearing opcodes** — DONE (v1.2.0)~~
~~3. **ReparentWindow** — DONE (v1.2.0)~~
~~4. **xcalc button outlines + routing** — DONE (v1.3.0): server-drawn borders + button routing fix~~
~~5. **GC function/planemask enforcement** — DONE (v1.4.0)~~
~~6. **BIG-REQUESTS extension** — DONE (v1.4.0), advertised and functional~~
~~7. **16-bit text** — DONE (v1.4.0): PolyText16, ImageText16~~
~~8. **WarpPointer** — DONE (v1.4.0): opcode 41 via CGWarpMouseCursorPosition~~
~~9. **Extension stubs** — DONE (v1.4.0): handler code for RENDER, XFIXES, SHAPE, RANDR, Xinerama, GE (NOT yet advertised)~~
~~10. **Selections/clipboard** — DONE (v1.5.0): macOS ↔ X11 clipboard bridge~~
~~11. **GC fill-style rendering** — DONE (v1.5.0): Tiled/Stippled/OpaqueStippled fills~~
~~12. **Bidirectional clipboard sync** — DONE (v1.5.1): X11→macOS + macOS→X11~~
~~13. **CWBackPixmap/ParentRelative** — DONE (v1.5.2)~~
~~14. **ConfigureWindow CWStackMode** — DONE (v1.5.6): full sibling stacking~~
~~15. **RENDER extension** — DONE (v1.6.0): glyph storage + CompositeGlyphs rendering + Trapezoids + all Porter-Duff blend modes + mask support. Advertised to clients.~~
~~16. **XFIXES extension** — DONE (v1.6.0): cursor, region, shape stubs. Advertised to clients.~~
~~17. **SHAPE extension stubs** — DONE (v1.6.0): ShapeRectangles/Mask/QueryExtents. NOT advertised (breaks xeyes).~~

~~18. **Font infrastructure** — DONE (v1.7.0): XLFD wildcard matching, PCF font support, system directory scanning, ListFontsWithInfo, Symbol encoding~~

~~19. **RANDR, Xinerama, GE extensions** — DONE (v1.7.4): All three advertised. RANDR v1.3 single-screen stubs (GetScreenResources, GetOutputInfo, GetCrtcInfo, etc.). Xinerama single-screen. GE v1.0 QueryVersion.~~

~~20. **SHAPE extension actual clipping** — DONE (v1.7.5): Full SHAPE implementation with ShapeRegion storage, wire protocol parsing (ShapeRectangles/Mask/Combine/Offset), depth-1 pixmap drawing (PolyFillArc/PolyFillRectangle), hit testing, present-time alpha masking, Metal transparency. xeyes displays with transparent eye-shaped windows. 7 extensions now advertised.~~

~~21. **Multi-monitor support** — DONE (v1.9.7): ScreenLayout cache (CGGetActiveDisplayList + CGDisplayRegisterReconfigurationCallback), dynamic RANDR (per-monitor outputs/CRTCs/modes), dynamic Xinerama (per-monitor screens), virtual desktop coordinate conversion, root geometry fix, WarpPointer fix, OR window present retry. `xrandr --query` works.~~

### Remaining priorities
1. ~~**Complete Phase 3.3**~~ — ✅ Done (v1.8.0): CoreText font bridge + Xft verification
2. ~~**Window close → client kill**~~ — ✅ Done (v1.9.0): WM_DELETE_WINDOW ClientMessage + forceful disconnect fallback (Phase 5.4)
3. ~~**Error handling**~~ — ✅ Done (v1.9.3): X11 error generation across ~50 handlers in 14 files (Phase 4.1)
4. ~~**Container networking**~~ — ✅ Done (v1.9.4): TCP + Unix socket multi-listener for Docker workflow (Phase 5.2)
5. ~~**Multi-monitor support**~~ — ✅ Done (v1.9.7): ScreenLayout cache, dynamic RANDR/Xinerama, coordinate fixes, OR window present retry

### Phase 2+3 Status Assessment (v1.7.5)
**Phase 2 is complete; Phase 3 is mostly complete.** All RENDER operations are implemented: Composite (with mask + gradient sources), CompositeGlyphs8/16/32, Trapezoids, Triangles/TriStrip/TriFan, FillRectangles, all Porter-Duff blend modes, gradient source pictures (Linear/Radial/Conical). **Seven extensions now advertised**: BIG-REQUESTS, RENDER, XFIXES, RANDR (v1.3 single-screen), XINERAMA (1 screen), GE (v1.0), and SHAPE (v1.1 with actual clipping). SHAPE is fully implemented with ShapeRegion storage, depth-1 pixmap drawing, hit testing, and present-time alpha masking — xeyes displays with transparent eye-shaped windows. Fonts: XLFD wildcard matching, PCF font loading, Symbol encoding, ListFontsWithInfo all work. 3.3 (TrueType/CoreText) has unchecked items (client-side Xft/fontconfig verification, optional CoreText bridge).

**v1.7.2 adds a reply-tracking safety net** that prevents XCB sequence desync crashes (the xcalc resize crash). Missing replies for reply-bearing opcodes now automatically get a BadImplementation error response.

**v1.7.3 adds a monotonic wire-sequence floor** that prevents event sequence regression when drainHostCommands() interleaves with readAndDispatch(). Also fixes button borders disappearing after window resize.

**v1.7.4 enables RANDR, Xinerama, GE** — Phase 2.5 complete. RANDR stubs report single-screen configuration with 1 output/CRTC/mode. All commonly-queried RANDR sub-opcodes handled.

**v1.7.5 enables SHAPE with actual clipping** — Phase 2.4 complete. Full SHAPE wire protocol parsing (ShapeRectangles/Mask/Combine/Offset), ShapeRegion storage with Set/Union/Intersect/Subtract/Invert operations, depth-1 pixmap drawing for PolyFillArc/PolyFillRectangle, hit testing in InputRouting and XProtoNotifyBridge, present-time alpha masking with premultiplied alpha, Metal transparency with layer hierarchy management, shaped window resize handling.

**v1.8.0 adds CoreText font bridge** — Phase 3.3 complete. X11 font requests mapped to macOS system fonts (Menlo, Helvetica, Courier, etc.) via CoreText C API. Both 1-bit crisp and 8-bit antialiased rendering with runtime toggle in Preferences → Rendering. CoreText tried first in font lookup, PCF/BDF fallback for cursor/symbol fonts.

**v1.8.2 fixes descender clipping** — Glyph positioning formula was `topY = y - bbx_yoff - (bbx_h - 1)`, placing every glyph 1px too low vs X.org reference (`topY = y - bbx_yoff - bbx_h`). Fixed at all 4 text handlers. Also fixed CompositeGlyphs source picture resolution for old Xft 1×1 Repeat pixmap pattern.

**v1.8.3 fixes Xft subpixel glyph parsing** — AddGlyphs didn't handle ARGB32 glyph format (used for LCD/subpixel rendering). `alphaBpp()` returned 8 for ARGB32, causing A8 parser to read 1/4 of the bitmap data and corrupt all subsequent glyphs. Fixed with 32bpp parsing path. RENDER trace category added to TraceDefs.hpp.

**v1.9.0 adds window close → client kill** — Red close button and Cmd+W now terminate X11 clients. ICCCM-compliant: checks WM_PROTOCOLS property for WM_DELETE_WINDOW atom, sends ClientMessage event to allow graceful shutdown. Falls back to forceful socket disconnect (`removeClient()`) for clients that don't set WM_DELETE_WINDOW. New `HostCmdType::WindowClose` host command + `x11_post_window_close()` bridge function. `windowWillClose` in Swift now routes through the close protocol instead of raw `x11_post_window_destroy`.

**v1.9.3 adds X11 error handling** — Proper error generation (BadWindow, BadDrawable, BadPixmap, BadFont, BadAtom, BadColor, BadValue) across ~50 request handlers in 14 files. Three priority tiers: reply-bearing (critical for XCB sequence desync prevention), void resource-modifying (spec compliance), and drawing ops. Drawing ops check drawable existence to avoid false errors on valid-but-unresolvable drawables (depth-1 pixmaps, unmapped windows).

**v1.9.4 adds container/network support** — Multi-listener architecture for Docker workflow. XProtoDaemon supports simultaneous TCP (0.0.0.0 bind) and Unix domain socket (/tmp/.X11-unix/X{display}) listeners. Settings UI wired with TCP/Unix toggles and Docker usage instructions. New `x11_start_server_ex()` bridge function.

**v1.9.5 adds window management attributes** — Full parsing, storage, and reporting of override_redirect (bit 9), win_gravity (bit 5), bit_gravity (bit 4), and backing_store (bit 6). Override-redirect windows create borderless floating NSWindows that don't steal focus.

**v1.9.6 adds UX bug fixes** — Ctrl+click → button 3 (right-click), window persistence fix (occluded windows no longer unmapped), QueryTree root children support.

**v1.9.7 adds multi-monitor support** — ScreenLayout cache queries CGGetActiveDisplayList for real per-monitor data (position, size, physical mm, pixel dimensions). Dynamic RANDR: RRGetScreenResources/Current reports N outputs, N CRTCs, N modes. RRGetOutputInfo (36-byte reply with correct nClones/nameLength). RRGetCrtcInfo returns per-monitor position/size/mode. New handlers: RRGetCrtcTransform (identity, 96-byte reply), RRGetPanning/SetPanning, RRGetProviders. Fixed 4 wrong RANDR minor opcode case numbers (19→25, 13→20, 14→21, 15→22). Dynamic Xinerama: QueryScreens returns N real screens. X11↔macOS coordinate conversion uses virtual desktop union bounds (all screens, not just NSScreen.main). GetGeometry on root returns actual virtual desktop dimensions. WarpPointer uses virtual desktop bounds. Override-redirect window Metal drawable retry mechanism (up to 5 retries when drawable isn't ready after cross-screen setFrame). `xrandr --query` now works and reports real monitor configuration.

6. ~~**Rendering performance**~~ — ✅ Done (v1.10.0): Metal-only rendering (software path removed), PutImage bulk memcpy, Expose two-pass (Phase 5.1)
7. ~~**Multi-monitor popup fix**~~ — ✅ Done (v1.10.7): Fix blank popup text on external monitors (contentsScale), hot-plug popup positioning (server-side adjustment + X11 position sync)
8. ~~**Keyboard support**~~ — ✅ Done (v1.11.0): Full keysym table, 4-column GetKeyboardMapping, 2-key GetModifierMapping, real QueryKeymap (Phase 5.3)
9. ~~**Window menu + title sync**~~ — ✅ Done (v1.11.1): macOS Window menu, WM_NAME/_NET_WM_NAME title sync (Phase 5.5)
10. ~~**ICCCM/WM compliance**~~ — ✅ Done (v1.12.0): WM_NORMAL_HINTS, WM_HINTS, WM_TAKE_FOCUS, EWMH window types/state, minimum size floor (Phase 5.7)
11. ~~**Vivado banner + menu fixes**~~ — ✅ Done (v1.12.2): Deferred show for floor-sized windows, ConfigureNotify on user window drag
12. **Vivado confirmed working** (v1.12.2): Full GUI with menus, dialogs, banner — all functional on multi-monitor setup

**v1.10.0 adds rendering performance** — Metal-only rendering (software CGImage/CALayer path removed entirely). PutImage bulk memcpy for GXcopy fast path. Expose two-pass restructuring (backgrounds/borders first, then Expose events).

**v1.10.7 fixes multi-monitor popup menus** — Three bugs fixed: (1) Blank popup text on external monitors — `CAMetalLayer.contentsScale=0.0` for borderless NSWindows, fixed by explicit backingScaleFactor propagation. (2) Hot-plug popup positioning — xterm doesn't query RANDR so Xlib's WidthOfScreen/HeightOfScreen remain stale after monitor changes, causing popups on wrong screen. Fixed with server-side `adjustOROriginForCursorScreen()` + `x11_set_window_position()` to sync X11 geometry. (3) ScreenLayoutChanged host command broadcasts ConfigureNotify + RRScreenChangeNotify on display reconfiguration.

**v1.11.0 adds comprehensive keyboard support** — Full macOS virtual keycode → X11 keysym mapping. 4-column GetKeyboardMapping, 2-key GetModifierMapping, real QueryKeymap.

**v1.11.1 adds Window menu + WM_NAME title sync** — macOS Window menu via NSApp.windowsMenu, WM_NAME/`_NET_WM_NAME` → NSWindow title.

**v1.12.0 adds ICCCM/WM compliance** — Phase 5.7 complete. WM minimum size floor (200×100 for top-level windows). Full WM_NORMAL_HINTS parser (PSize, PMinSize, PMaxSize, PResizeInc → NSWindow contentMinSize/contentMaxSize/contentResizeIncrements). WM_HINTS (InputHint → wants_input, StateHint → miniaturize on map). WM_TAKE_FOCUS protocol (focus delivery via ClientMessage when client advertises it). EWMH: `_NET_WM_WINDOW_TYPE` → NSWindow style mapping (NORMAL/DIALOG/TOOLBAR/UTILITY/MENU/TOOLTIP/SPLASH), `_NET_WM_STATE` (MODAL/FULLSCREEN), `_NET_FRAME_EXTENTS` set proactively. Pre-registered atoms 78-92.

**v1.12.2 fixes Vivado banner + cross-monitor menus** — (1) Banner race fix: floor-sized (200×100) windows stay hidden until applyX11Resize, first present, or 500ms timeout (`pendingNonORShow` deferred show mechanism). (2) ConfigureNotify on user window drag: new `WindowMoved` HostCmdType sends ConfigureNotify when user drags NSWindow, fixing Java/Swing stale root coordinate cache that broke menu tracking after cross-monitor window moves. Vivado confirmed working.

**v1.13.0 adds Vitis extension stubs + UX fixes** — XFIXES minor 1/2 and RANDR minor 15 handlers eliminate BadRequest errors from Vitis. Multi-monitor window placement fix (`adjustNonOROriginForMainScreen`). Dynamic View menu (Show/Hide Log Window toggle). Graceful quit with connected clients (`applicationShouldTerminate` stops X11 server before teardown). Deferred show retry loop for floor-sized windows.

**v1.13.2 adds app icon + Stage Manager fix** — Custom app icon from SwiftX11 logo PDF (10 sizes, blue background). Bundle ID changed to `com.rlan.SwiftX11` (fixes poisoned icon cache + proper reverse-DNS). NSWindow.sharingType = .readWrite for window server capture.

**v1.14.0 adds Help menu + About dialog** — Phase 6 complete. HelpView.swift with comprehensive user documentation (DISPLAY setup, Docker, fonts, settings, keyboard/mouse, copy & paste, extensions, limitations). About dialog with version, build date, credits. All menus via SwiftUI CommandGroups (About, View toggle, Help) with AppDelegate only for Help Book neutralization + Window menu adoption.

**v1.14.1 adds XC-MISC extension** — Phase 7.1 complete. XC-MiscGetVersion (1.1), XC-MiscGetXIDRange (per-client midpoint-up allocation), XC-MiscGetXIDList (capped at 4096). Major opcode 140. Total advertised extensions: 8. Prevents XID exhaustion crash for long-running Vivado sessions.

**v1.14.2 adds XInput2 + XTEST extensions** — Phase 7.2 + 7.3 complete (stubs). XInput2: XIQueryVersion (2.0), XIQueryDevice (4 virtual core devices with ButtonClass/KeyClass), XISelectEvents (silently accepts). XTEST: XTestGetVersion (2.2), XTestFakeInput (stub), XTestGrabControl (stub). Major opcodes 141/142. Total advertised extensions: 10. GTK3 confirmed working via Docker (zenity with zero errors).

**v1.15.x completes XI2 events + Shape AA + bug fixes**:
- **v1.15.4**: Full XI2 event delivery — 6 event senders (Motion, Button, Key, Crossing, Focus, RawMotion) with 13 injection points. RawMotion uses 68-byte GenericEvent with valuator data. `first_event=93` prevents libXi from overwriting core event handlers. xeyes works with XI2 advertised.
- **v1.15.3**: `GlobalPointerTracker` (NSEvent global+local monitor) delivers RawMotion globally so xeyes tracks cursor even outside X11 windows.
- **v1.15.5**: Shape AA compositing with straight alpha (3×3 box-filter, cardinal-neighbor fast-path). Previous premultiplied attempt reverted.
- **v1.15.6**: Clipboard thread-safety fix — NSPasteboard callbacks dispatched to main thread via `DispatchQueue.main.sync`. Fixes Vivado Edit→Copy hang (deadlock between xproto thread and main thread).
- **v1.15.7**: Debug trace cleanup (~50 `#ifndef NDEBUG` fprintf removed). Vivado startup banner race fix — present path no longer shows window while still at WM floor size (200×100).
- **v1.15.15**: MotionNotify coalescing — HostCommandQueue deduplicates consecutive same-window PointerMove commands at push time, preventing motion event flood that live-locked Java AWT's XAWT thread (held AWT lock in XPending loop, starved EDT from clipboard ops).
- **v1.15.17**: Vivado Edit→Copy hang fix — UngrabKeyboard now sends FocusOut(mode=Ungrab) + FocusIn(mode=Ungrab) per X11 spec. Java AWT required these to release the AWT lock after menu dismiss. Root cause identified via xscope wire comparison with XQuartz.
- **v1.15.18**: PropertyNotify on ChangeProperty — X11 spec requires PropertyNotify (type 28) whenever a property changes. Java AWT writes `_SUNW_JAVA_AWT_TIME` via ChangeProperty(Append) and waits for PropertyNotify to extract server timestamp for SetSelectionOwner(CLIPBOARD). Without it, Java blocked after InternAtom("CLIPBOARD").
- **v1.15.19-20**: Proactive clipboard capture — on SetSelectionOwner(CLIPBOARD or PRIMARY), server sends SelectionRequest(UTF8_STRING) to new owner, intercepts SelectionNotify response, pushes content to NSPasteboard. Enables Vivado Edit→Copy → macOS Cmd+V and xterm select → macOS Cmd+V.
- **v1.15.22**: PropertyNotify event mask filtering — only sent to windows with PropertyChangeMask (bit 22) selected. Previously sent unconditionally, flooding Java with hundreds of type=28 events during Vivado startup.

**Known issue (v1.15.22)**: Vivado menu item highlighting is sluggish (many-second delays). Menus use lightweight popups (rendered within main window, not OR windows). Only ~8 MotionNotify events delivered during menu grab. Needs xscope comparison with XQuartz.

**Next priorities**: (1) Fix Vivado menu highlighting, (2) Clean up diagnostic traces, (3) Update CLAUDE.md, (4) Vitis testing (Phase 8).

### Bug fixes (v1.15.17-22)
- **Vivado Edit→Copy hang — missing FocusIn/FocusOut on UngrabKeyboard** (v1.15.17): UngrabKeyboard handler was a silent no-op. X11 spec requires FocusOut(mode=Ungrab) to the grab window and FocusIn(mode=Ungrab) to the focus window when a keyboard grab is released. Java AWT's XAWT thread waited for these events before releasing the AWT lock, blocking the EDT from executing the clipboard Copy action. Diagnosed via xscope wire comparison: XQuartz sends both focus events; SwiftX11 didn't. Fix: GrabOps.cpp UngrabKeyboard handler now sends FocusOut(detail=Nonlinear, mode=Ungrab) + FocusIn(detail=Nonlinear, mode=Ungrab) using `sendFocusEventDirect()`.
- **Vivado Edit→Copy hang — missing PropertyNotify on ChangeProperty** (v1.15.18): After the FocusIn/Out fix, Java proceeded past the ungrab but hung after writing `_SUNW_JAVA_AWT_TIME` via ChangeProperty(Append). Java AWT waits for the PropertyNotify event to extract the server timestamp needed for SetSelectionOwner(CLIPBOARD, time). X11 spec: server must generate PropertyNotify (type 28) whenever a property is changed. Fix: PropOps.cpp now calls `sendPropertyNotify()` after every ChangeProperty, gated by PropertyChangeMask (bit 22) on the window's event_mask (v1.15.22).
- **Vivado clipboard content not reaching macOS** (v1.15.19-20): After the hang was fixed, Edit→Copy succeeded on the X11 side but text didn't reach NSPasteboard. Root cause: no mechanism to proactively capture selection content when a client calls SetSelectionOwner. Fix: on SetSelectionOwner(CLIPBOARD or PRIMARY), server sends SelectionRequest(UTF8_STRING) to the new owner, intercepts the SelectionNotify response in handleSendEvent, and pushes content to NSPasteboard. Root proxy ownership (XID 1) assigned after capture so subsequent X11 ConvertSelection routes through server.
- **PropertyNotify event flooding** (v1.15.22): PropertyNotify was sent unconditionally for every ChangeProperty, flooding Java with hundreds of type=28 events during Vivado startup. Fix: sendPropertyNotify now checks `wv.event_mask & PropertyChangeMask` before sending.
- **MotionNotify flood causing AWT lock starvation** (v1.15.15): After menu ungrab, hundreds of MotionNotify events queued in the socket prevented Java's XAWT thread from releasing the AWT lock (tight `while (XPending > 0)` loop). Fix: HostCommandQueue coalesces consecutive same-window PointerMove commands at push time, keeping only the last position.

### Bug fixes (v1.6.0)
- **whitePixel fix**: X11 setup reply was sending whitePixel=0x00000000 instead of 0x00FFFFFF. Fixed in X11Setup.cpp.
- **xeyes resize stale outlines**: Old eye outlines persisted after window resize. Fixed by clearing drawable in ClearArea before redraw.
- **xterm stale bottom pixels**: Black pixels accumulated at bottom of text rows during scrolling. Root cause: ImageText8/16 background fill was 1 pixel too short (fontAscent + fontDescent covered [y-ascent, y+descent) exclusive, but glyphs with bbx_yoff=-fontDescent place their bottom pixel at exactly y+descent). Fixed by extending background fill to fontAscent + fontDescent + 1.

### Bug fixes (v1.7.0)
- **xcalc hang on ListFontsWithInfo**: Terminator reply used sendReply32 (32 bytes) but declared length=7 (28 more bytes expected). Client blocked forever waiting for the remaining 28 bytes. Fixed by using sendReplyRaw to send full 60-byte terminator.
- **RENDER damage clamping**: Composite and FillRectangles reported damage using unclipped request coordinates while pixel writes were clamped to drawable bounds. Caused unnecessary full-frame Metal uploads. Fixed by clamping damage rects to match actual pixel-write regions.
- **PCF LSB-first bit order**: Added byte-level bit reversal for PCF fonts with LSB-first bitmap bit order. Uses 256-entry lookup table with thread_local buffer.
- **App sandbox blocked PCF font loading**: `ENABLE_APP_SANDBOX=YES` in Xcode injected sandbox entitlements at code signing, blocking `std::ifstream` from opening `/opt/X11/share/fonts/*/fonts.dir` (errno=1 EPERM). Result: 0 PCF fonts registered, all OpenFont fell through to builtin "fixed" Latin-1 font. Fixed by setting `ENABLE_APP_SANDBOX=NO`.
- **Font aliases not resolving**: `loadAliases()` only checked builtin BDF fonts for alias targets. Most alias targets in fonts.alias point to PCF fonts. Fixed by adding PCF registry lookup (exact + glob match). Aliases resolved: 0 → 99.
- **xterm scrollbar vanishes on resize**: First ExposeChildren at windowDidEndLiveResize fired before xterm reconfigured children. Right-side scrollbar at old x exceeded new surface width → resolveDrawableRW failed silently (effW=0). Fixed by adding second ExposeChildren after promoteDisplaySurface (150ms post-resize).

### Features (v1.7.1)
- **Triangles/TriStrip/TriFan**: RENDER geometric fill ops (minor 11/12/13). Scanline triangle rasterization with FIXED 16.16 edge interpolation.
- **Gradient source pictures**: CreateLinearGradient, CreateRadialGradient, CreateConicalGradient now create real gradient Pictures. Composite samples gradient per-pixel with color stop interpolation.

### Features (v1.7.4)
- **RANDR extension advertised**: QueryExtension("RANDR") now returns present=1, major opcode 136. RRQueryVersion returns 1.3. Originally single-screen stubs (v1.7.4), upgraded to dynamic multi-monitor (v1.9.7) — see v1.9.7 features below.
- **Xinerama extension advertised**: QueryExtension("XINERAMA") and alias "PANORAMIX" now return present=1, major opcode 137. XineramaQueryVersion returns 1.1, IsActive=1. Originally single-screen (v1.7.4), upgraded to dynamic multi-monitor (v1.9.7).
- **Generic Event Extension (GE) advertised**: QueryExtension("Generic Event Extension") now returns present=1, major opcode 138. GEQueryVersion returns 1.0.
- **Total advertised extensions**: 6 (BIG-REQUESTS, RENDER, XFIXES, RANDR, XINERAMA, GE). Updated to 7 in v1.7.5 (adds SHAPE).

### Features (v1.7.5)
- **SHAPE extension actual clipping**: Full SHAPE implementation enabling non-rectangular windows with transparent backgrounds. xeyes now displays with transparent eye-shaped windows on macOS.
  - **ShapeRegion data structure**: Per-window bounding/clip/input shape storage with Set/Union/Intersect/Subtract/Invert operations. Bitmap→rectangles via run-length encoding with LSBFirst bit order.
  - **Wire protocol parsing**: ShapeRectangles (minor 1), ShapeMask (minor 2), ShapeCombine (minor 3), ShapeOffset (minor 4) all parse and store shape data. ShapeQueryExtents (minor 5) and ShapeGetRectangles (minor 8) return real data.
  - **Depth-1 pixmap drawing**: PolyFillArc and PolyFillRectangle handle depth-1 pixmap targets with per-bit manipulation. Required because xeyes creates elliptical masks via XFillArc on depth-1 bitmaps.
  - **Hit testing**: InputRouting.cpp and XProtoNotifyBridge.cpp check shape containment after rectangular bounds check. Input shape checked first, falls back to bounding shape.
  - **Present-time alpha masking**: Swift applies shape mask at present time — saves original data, zeros all pixels (premultiplied transparent), copies back shape-interior pixels with forced alpha.
  - **Metal transparency**: NSWindow.isOpaque=false, entire view hierarchy set non-opaque (SwiftUI NSHostingView inserts intermediate layers), CAMetalLayer.isOpaque enforced at present time, Metal pipeline uses alpha blending (sourceAlpha/oneMinusSourceAlpha).
  - **Shaped window resize**: Retained display buffer skipped for shaped windows (old-size frame + old-size mask = distortion), new surfaces filled with transparent black.
  - **Known issue**: Eyes occasionally flash black during live resize.

### Bug fixes (v1.7.3)
- **xcalc resize crash — monotonic wire-sequence floor**: Even with v1.7.2's reply-tracking safety net, xcalc resize still triggered `[xcb] Unknown sequence number` crashes. Root cause: `drainHostCommands()` interleaves with `readAndDispatch()`, and host commands (resize → ConfigureNotify, Expose) carry stale sequences from `lastSeq()` that are behind sequences already sent during client request dispatch. XCB widens 16-bit sequences to 64-bit monotonically — any backwards sequence causes crash. Fixed by tracking `max_wire_seq_` (highest sequence ever sent) in `XProtoTransport::sendAll()` and bumping any stale sequence forward. Payload-aware: `payload_remaining_` counter distinguishes reply payload chunks (bytes[2:3] are arbitrary data) from response headers (bytes[2:3] are sequence numbers).
- **Button borders missing after resize**: After window resize, button outlines (server-drawn borders) disappeared — only text labels rendered. Root cause: `ExposeChildren` handler was "non-destructive" (only sent Expose events), but after resize the surface is cleared to white, so server-drawn borders and backgrounds were lost. Fixed by adding `fillWindowBorderIfReady()` and `fillWindowBackgroundIfReady()` calls in `ExposeChildren`, matching `sendExposeSubtree` behavior.

### Bug fixes (v1.8.2)
- **Descender clipping**: Characters with descenders (g, j, p, q, y) had bottom pixels clipped. Root cause: glyph positioning formula `topY = y - bbx_yoff - (bbx_h - 1)` placed every glyph 1 pixel too low vs X.org reference (`topY = y - bbx_yoff - bbx_h`). The bottom pixel of maximum-descent glyphs extended below ImageText8 background fill, getting overwritten by the next line's background. Fixed at all 4 text handlers (PolyText8/16, ImageText8/16).
- **CompositeGlyphs source color defaulting to black**: When old-style Xft used a 1×1 Repeat pixmap (instead of CreateSolidFill) for text color, CompositeGlyphs defaulted srcColor to black (0xFF000000). Fixed by adding 1×1 Repeat → solid promotion logic matching the Composite handler.
- **CoreText descent workaround removed**: The `+1` descent padding in CoreTextFont.cpp was compensating for the same off-by-one; no longer needed.

### Bug fixes (v1.8.3)
- **Xft garbled text with Menlo 16pt** (`xterm -fa Menlo -fs 16`): ARGB32 glyph format (subpixel/LCD rendering) not handled in AddGlyphs. `alphaBpp()` returned 8 for unrecognized formats, causing A8 parser to read 1 byte per pixel instead of 4. This consumed 1/4 of each glyph's bitmap data; the remaining 3/4 was misinterpreted as subsequent glyph metadata, corrupting all glyphs after the first in each AddGlyphs batch. Fixed by adding 32bpp path to `alphaBpp()` and AddGlyphs parser (extracts alpha channel from ARGB32 pixels).
- **RENDER trace category**: Added `X11_TRACE_RENDER_ENABLED` to TraceDefs.hpp categorical trace system. Migrated ad-hoc `#define X11_TRACE_RENDER` and `#ifndef NDEBUG` debug traces to use the standardized flag. Font debug traces (IT8_DBG, PT8_DBG, CA_DBG) moved from `#ifndef NDEBUG` to `X11_TRACE_FONT_ENABLED`.

### Bug fixes (v1.7.2)
- **xcalc resize crash (XCB sequence desync)**: Resizing xcalc triggered `[xcb] Unknown sequence number while processing queue` → abort. Root cause: a reply-bearing request was dispatched to a handler that didn't send a reply, causing XCB's internal sequence tracking to desync. Fixed with three-layer defense:
  1. **Reply-sent tracking**: `XProtoTransport::reply_sent_` flag set in `sendAll()` when sending a reply (byte[0]==1) or error (byte[0]==0). Reset before each dispatch.
  2. **Core opcode safety net**: `isReplyBearingCore()` 128-entry static table in `XProtoServer::dispatch()`. After dispatch, if reply-bearing and no reply sent, sends `BadImplementation` error. Covers: unregistered handlers, handler exceptions, and normal dispatch without reply.
  3. **Extension error replies**: All extension `default` switch cases (XFIXES, SHAPE, RANDR, Xinerama, GE, RENDER) now send `BadRequest` error instead of silently consuming bytes.

### Features (v1.9.0)
- **Window close kills client**: Red close button and Cmd+W now terminate X11 clients instead of just hiding the NSWindow. ICCCM-compliant: reads WM_PROTOCOLS property on the window, checks for WM_DELETE_WINDOW atom. If present, sends ClientMessage event (type=WM_PROTOCOLS, data[0]=WM_DELETE_WINDOW, data[1]=timestamp) so the client can exit gracefully. If WM_DELETE_WINDOW not in WM_PROTOCOLS, forcefully disconnects the client by closing the socket (same path as `removeClient()`). New `HostCmdType::WindowClose` host command type, `x11_post_window_close()` bridge function, and `windowSupportsDeleteProtocol()`/`sendDeleteWindowMessage()` helpers in XProtoDaemon.cpp. Atoms already pre-registered: kWM_PROTOCOLS=76, kWM_DELETE_WINDOW=77.

### Features (v1.10.0)
- **Metal-only rendering**: Software (CGImage/CALayer) rendering path removed entirely — Metal is now required. Removed `setupSoftwareLayer()`, `presentSoftware()`, `presentSoftwarePartial()`, `makeCGImage()`, `imageLayer`, `usingMetal`/`wantsMetal` flags, and "Use Metal Rendering" settings toggle. `setUseMetal()` replaced with `ensureMetalSetup()` (lazy one-time init).
- **PutImage bulk memcpy**: GXcopy fast path uses `std::memcpy` for row data then 4-pixel-unrolled alpha forcing, instead of per-pixel copy-and-OR.
- **Expose two-pass**: `sendExposeSubtree` and `ExposeChildren` restructured to fill all backgrounds/borders first, then send Expose events. Prevents background fill from overwriting sibling content during re-expose.

### Bug fixes (v1.10.7)
- **Blank popup text on external monitors**: Override-redirect (popup) windows on external monitors rendered blank — text was present but invisible. Root cause: `CAMetalLayer.contentsScale` stayed at 0.0 for borderless NSWindows on external monitors (AppKit doesn't auto-inherit `backingScaleFactor` for `.borderless` style mask). Fixed by explicitly setting `contentsScale = window.backingScaleFactor` in `ensureMetalSetup()` and updating on `viewDidChangeBackingProperties`.
- **Hot-plug monitor popup positioning**: After monitor hot-plug/unplug, popup menus appeared on the wrong screen because xterm doesn't query RANDR (Xlib caches `WidthOfScreen`/`HeightOfScreen` from connection setup permanently). Fixed with three-part approach: (1) `ScreenLayoutChanged` host command broadcasts ConfigureNotify + RRScreenChangeNotify to all clients on display reconfiguration (helps RANDR-aware clients); (2) Server-side `adjustOROriginForCursorScreen()` detects when a popup would land on a different macOS screen than the cursor and repositions it; (3) `x11_set_window_position()` syncs X11 WindowTable geometry after adjustment so input event coordinates (event_xy = root_xy - window_origin) remain consistent.
- **Diagnostic trace cleanup**: Removed ~217 lines of verbose per-frame OR popup traces (OR_RENDER, OR_METAL_UPLOAD, OR_TEXT, OR_SNAP, OR_PRESENT*). Kept lifecycle and error traces gated behind `#ifndef NDEBUG`.

### Features (v1.11.0)
- **Comprehensive keyboard support**: Full macOS virtual keycode → X11 keysym mapping covering all keys: letters (US layout), digits, punctuation, F1-F20, navigation keys, keypad, modifiers (left+right), CapsLock, Fn→Meta_L, ISO Section key. GetKeyboardMapping returns 4 keysyms per keycode (normal/shift/mode_switch/mode_switch+shift) for Java Swing/GTK. GetModifierMapping returns 2 keys per modifier (left+right). QueryKeymap returns real pressed-key state. ~90 keysym constants added.

### Features (v1.11.1)
- **Window menu integration**: macOS Window menu via `NSApp.windowsMenu` — AppKit auto-lists all X11 NSWindows.
- **WM_NAME title sync**: WM_NAME (atom 39) and `_NET_WM_NAME` (atom 79) property changes trigger NSWindow title update via `x11_ui_push_title()`. Child window titles route through `topLevelAncestorOf()`.

### Features (v1.12.0)
- **ICCCM/WM compliance suite** (Phase 5.7):
  - **Minimum window size floor**: Top-level windows created below 200×100 enlarged in C++ CreateWindow. Swift NSWindow applies matching floor + contentMinSize(100×50).
  - **WM_NORMAL_HINTS**: Full ICCCM XSizeHints parser (PSize, PMinSize, PMaxSize, PResizeInc, PBaseSize). Auto-resizes tiny windows. Push to Swift → NSWindow contentMinSize/contentMaxSize/contentResizeIncrements.
  - **WM_HINTS**: Parses InputHint (wants_input stored in WindowView), StateHint (IconicState → miniaturize on map). Icon hints skipped.
  - **WM_TAKE_FOCUS**: Focus handler sends ClientMessage(WM_PROTOCOLS, WM_TAKE_FOCUS, timestamp) before FocusIn when advertised. `wants_take_focus` cached in WindowView for fast lookup.
  - **_NET_WM_WINDOW_TYPE**: NSWindow style mapping — DIALOG (titled+closable, floating if transient), TOOLBAR/UTILITY (titled+closable, floating), MENU/TOOLTIP (borderless+floating), SPLASH (borderless+centered+floating), NORMAL (default).
  - **_NET_WM_STATE**: MODAL → modalPanel level, FULLSCREEN → toggleFullScreen.
  - **_NET_FRAME_EXTENTS**: Set proactively on MapWindow (left=0, right=0, top=28 for titled, bottom=0).
  - Pre-registered atoms 78-92 (WM_TAKE_FOCUS through _NET_WM_STATE_FULLSCREEN).

### Bug fixes (v1.12.2)
- **Vivado banner race condition**: Floor-sized (200×100) windows briefly appeared at minimum size before real ConfigureWindow arrived. Root cause: mapWindow called makeKeyAndOrderFront before client's ConfigureWindow/WM_NORMAL_HINTS set the actual size. Fixed with deferred show mechanism: `pendingNonORShow` set tracks floor-sized windows; show triggered by (a) applyX11Resize (ConfigureWindow arrived), (b) first present succeeds (client drew content), or (c) 500ms safety timeout. Window stays hidden until one of these triggers fires.
- **Cross-monitor menu tracking**: After dragging Vivado's main window from one monitor to another, menus appeared correctly but items didn't highlight or respond to clicks. Root cause: Java/Swing caches root window coordinates and only updates on ConfigureNotify. No ConfigureNotify was sent when user dragged the NSWindow. Fixed by adding `WindowMoved` HostCmdType: `x11_set_window_position()` (called from Swift `windowDidMove`) now pushes a WindowMoved host command; `XProtoDaemon::drainHostCommands` sends ConfigureNotify with updated x/y to the owning client. Skip no-op updates when position hasn't changed.
- **mapWindow geometry sync**: Before showing non-OR windows, mapWindow now queries X11 geometry via `x11_get_window_geometry()` and synchronously applies `setContentSize` + `setFrameOrigin` to avoid flash of wrong-sized/positioned window.
- **NSApp.activate on mapWindow**: Calls `NSApp.activate(ignoringOtherApps: true)` to ensure X11 windows appear above other macOS apps.

### Features (v1.15.5)
- **Window Shape AA compositing**: Straight alpha blending for SHAPE extension windows. Metal pipeline uses `sourceAlpha/oneMinusSourceAlpha`. Previous premultiplied attempt (v1.15.0) reverted due to double-application artifacts.

### Bug fixes (v1.15.15-22)
- **Vivado Edit→Copy hang — MotionNotify flood** (v1.15.15): After menu ungrab, hundreds of MotionNotify events in the socket prevented Java's XAWT thread from releasing the AWT lock. Fix: HostCommandQueue coalesces consecutive same-window PointerMove commands at push time.
- **Vivado Edit→Copy hang — missing FocusIn/FocusOut on UngrabKeyboard** (v1.15.17): UngrabKeyboard was a silent no-op. X11 spec requires FocusOut(mode=Ungrab) + FocusIn(mode=Ungrab). Java AWT waited for these events before releasing the AWT lock. Fix: GrabOps.cpp sends both focus events via `sendFocusEventDirect()`.
- **Vivado Edit→Copy hang — missing PropertyNotify** (v1.15.18): Java AWT waited for PropertyNotify after writing `_SUNW_JAVA_AWT_TIME` via ChangeProperty(Append). Fix: PropOps.cpp now calls `sendPropertyNotify()` after every ChangeProperty, gated by PropertyChangeMask (v1.15.22).
- **Vivado clipboard content not reaching macOS** (v1.15.19-20): After the hang was fixed, Edit→Copy succeeded X11-side but text didn't reach NSPasteboard. Fix: on SetSelectionOwner(CLIPBOARD or PRIMARY), server sends SelectionRequest(UTF8_STRING) to the new owner, intercepts SelectionNotify, and pushes content to NSPasteboard.

### Features (v1.17.0-v1.17.1)
- **SubstructureRedirect emulation**: Replaces timer-based deferred show with proper WM emulation for window sizing at map time. Three-layer approach:
  1. **Poll loop drain**: `readAndDispatch` returns `DispatchResult` enum; poll loop drains ALL buffered data per client before flushing pending maps. Allows ConfigureWindow + WM_NORMAL_HINTS to arrive before map is pushed to Swift.
  2. **Deferred map for tiny windows**: MapWindow on root children with geometry <50px calls `ctx.addPendingMap(wid)` instead of `x11_ui_push_map`. `flushPendingMaps()` called after drain.
  3. **Peak pre-map size tracking**: Tracks largest ConfigureWindow size seen per unmapped root child. At flush time, resolution order: WM_NORMAL_HINTS (ICCCM authoritative) → peak size (largest ConfigureWindow before map) → current geometry. Centers resized windows on primary monitor.
- **kWM_NORMAL_HINTS atom fix**: Constant corrected from 41 to 40 (was reading WM_SIZE_HINTS instead of WM_NORMAL_HINTS).
- **Double MAP_SHOW fix**: `wasMapped` guard in MapWindow handler prevents MapSubwindows + MapWindow from both pushing maps for the same host window, which caused the "genie effect" animation artifact.
- **CreateWindow size floor removed**: Windows stored at client's requested size. WM emulation at map time handles sizing, matching XQuartz/quartz-wm behavior.
- **`pushMapExtras()` helper**: Sends resize/move/_NET_FRAME_EXTENTS after map. Used by both immediate and deferred map paths.

---

## HIGH PRIORITY — Active Bugs

### JidePopup / Undecorated Window Handling — ✅ FIXED (v1.19.15–v1.19.24)
Java Swing JidePopup windows now appear as borderless floating popups (not decorated windows).

- [x] Investigate which hints JidePopup sets → uses `_MOTIF_WM_HINTS` with `decorations=0`
- [x] Honor `_NET_WM_WINDOW_TYPE` — already handled (v1.12.0, DIALOG/TOOLBAR/UTILITY/MENU/TOOLTIP/SPLASH)
- [x] Honor `_MOTIF_WM_HINTS` decoration flags — `decor=0` → borderless+floating+shadow (v1.19.15)
- [x] Borderless popups use `orderFront` (no focus steal, no `canBecomeKeyWindow` warning) (v1.19.20)
- [x] Click-through via `acceptsFirstMouse` on X11MTKView (v1.19.24)
- [x] xterm Ctrl+click menus (override-redirect) unaffected

### XTEST v2.2 GrabControl Crash — ✅ RESOLVED (v1.19.15)
Root cause: GTK2/GTK3 library conflict from `LD_PRELOAD=libgdk-x11-2.0.so.0` in Docker container. When XTEST v2.2 triggered AT-SPI initialization, GTK3's GDK loaded and tried to register `GdkDisplayManager` which GTK2 had already registered → duplicate type → segfault. Fix: remove GTK2 from `LD_PRELOAD`, fix dbus-launch ordering (DISPLAY must be set first). XTEST v2.2 now works correctly.

### Ctrl+Click → Right-Click Regression (MEDIUM — usability)
Ctrl+click no longer triggers context menus in Vivado (button 3 / right-click). Two-finger trackpad click works as a workaround. Need to identify which change in the v1.17→v1.19 range broke this. Ctrl+click still needs to work for xterm menus (Ctrl+Button1 = font menu, Ctrl+Button2 = VT options).

### Pointer Coordinate Offset After Left-Edge Resize — ✅ FIXED (v1.19.25)
Sync window position in `windowDidResize` before C++ resize path. Fixes stale origin in coordinate transform after left/top edge resize.

### Add Timestamps to Stderr Log — ✅ FIXED (v1.19.25)
C++ `TS_FPRINTF` macro with `mach_absolute_time()`. Removed unconditional Swift `print()` debug statements.

### Add Find/Search to Swift Log Window — ✅ FIXED (v1.19.25)
Cmd+F opens NSTextView find bar. Find toolbar button also works.

---

## Phase 8: X11 Protocol Hardening

Systematic review of all request handlers for spec compliance. Motivated by bugs in v1.15–v1.17 that stemmed from simplified implementations:
- `kWM_NORMAL_HINTS` atom off-by-one (was 41, should be 40)
- Missing `PropertyNotify` on `ChangeProperty` (broke Java AWT)
- Missing `FocusIn`/`FocusOut` on `UngrabKeyboard` (broke Java AWT)
- `SubstructureRedirect` race between `MapWindow` and `ConfigureWindow`
- `CreateWindow` size floor that didn't match WM behavior

### 8.1 Event Delivery Audit (HIGH) — MOSTLY COMPLETE (v1.18.1)
Ensure every request that must generate events actually does so per the X11 spec.

- [x] **PropertyNotify completeness**: `ChangeProperty` sends PropertyNotify(NewValue), `DeleteProperty` sends PropertyNotify(Deleted), both gated by PropertyChangeMask. RotateProperties still a stub.
- [x] **ConfigureNotify completeness**: `ConfigureWindow` sends ConfigureNotify to window (StructureNotifyMask) and parent (SubstructureNotifyMask) with correct above_sibling, override_redirect fields.
- [x] **MapNotify / UnmapNotify**: MapWindow, UnmapWindow, MapSubwindows, UnmapSubwindows all send MapNotify/UnmapNotify to window and parent with correct masks.
- [x] **CreateNotify / DestroyNotify**: CreateWindow sends CreateNotify to parent (SubstructureNotifyMask). DestroyWindow/DestroySubwindows send DestroyNotify to window and parent. (DestroySubwindows fixed v1.18.1.)
- [x] **ReparentNotify**: Sent to window, old parent, and new parent with correct override_redirect field.
- [ ] **GravityNotify**: Not implemented. Gravity attributes stored but gravity-based repositioning on resize not done (toolkits handle client-side). LOW priority.
- [x] **CirculateNotify**: CirculateWindow (opcode 13) fully implemented with RaiseLowest/LowerHighest and CirculateNotify to window and parent.
- [x] **FocusIn/FocusOut audit**: GrabKeyboard sends FocusOut/FocusIn(mode=Grab), UngrabKeyboard sends FocusOut/FocusIn(mode=Ungrab). DestroyWindow and UnmapWindow send FocusOut on focused window and clear focus. (v1.15.17 + v1.18.1.)
- [x] **Crossing events (EnterNotify/LeaveNotify)**: Correct mode and detail fields on normal motion and child-to-child transitions (v1.5.5).
- [ ] **Exposure events**: Expose events sent correctly. Count field passed but coalescing may not preserve individual counts. `GraphicsExposure`/`NoExposure` for CopyArea NOT implemented. LOW priority — no known client depends on this.
- [x] **SelectionClear**: SetSelectionOwner sends SelectionClear to previous owner with correct fields.
- [ ] **ColormapNotify**: Not implemented. Colormap is fixed default (TrueColor only). LOW priority — no toolkit needs this on TrueColor displays.

### 8.2 Error Generation Audit (HIGH) — MOSTLY COMPLETE (v1.19.27)
All requests must send the correct X11 error for invalid arguments, not silently skip.

Pre-existing coverage: 71 `sendErrorCore` calls across WindowOps, DrawOps, PropOps, QueryOps, ShapeOps, ExtensionOps, etc. (~65% of handlers).

- [ ] **BadWindow systematic check**: Every handler that takes a window XID must validate it and send `BadWindow` if not found (except root XID and None where allowed).
- [ ] **BadDrawable systematic check**: Drawing ops must validate drawable XIDs.
- [x] **BadDrawable in CopyPlane** (v1.19.26): CopyPlane now sends `BadDrawable` when source is neither window nor pixmap (was silent return).
- [ ] **BadPixmap**: `FreePixmap`, `CopyArea` with pixmap src/dst, tile/stipple GC values.
- [ ] **BadFont**: `QueryFont`, `QueryTextExtents` with invalid font ID.
- [ ] **BadGC in GCOps**: Reverted to lenient `getOrCreate` pattern — Java AWT creates/reuses GCs across connections. Strict validation kills Vivado. Leave lenient until full resource tracking is implemented.
- [ ] **BadCursor**: Reverted — Java AWT calls RecolorCursor on cursors not in our table (cursor `0x6000029` created by method we don't track). Strict BadCursor killed Vivado startup. Leave lenient.
- [x] **BadWindow in SetSelectionOwner** (v1.19.26): Validates owner window exists (unless 0=None or 1=root proxy).
- [x] **BadAtom**: `GetAtomName` validates and sends BadAtom (pre-existing). `InternAtom` correctly returns atom=None for only_if_exists. Property ops don't require atom validation per spec.
- [x] **BadValue enum clamping** (v1.19.27): GC function mask tightened 0xFF→0x0F. Gravity values clamped to 0-10, backing_store to 0-2. GC line_style/join_style/cap_style/fill_style/fill_rule/subwindow_mode/arc_mode already correctly masked. PutImage format validated 0-2 with BadValue error.
- [ ] **BadMatch**: Depth mismatch in `CopyArea`/`CopyPlane` — currently all surfaces are 32-bit so mismatch impossible. Spec compliance check deferred until multi-depth support.
- [x] **BadLength**: All major handlers check `br.remaining()` before reading fixed-size headers. No gaps found in audit.
- [ ] **BadAccess**: `ChangeWindowAttributes` on window not created by requesting client (for certain attributes). `SetSelectionOwner` timestamp validation. LOW — no known client triggers.
- [ ] **BadAlloc**: Memory allocation failures (pixmap too large, etc.) — currently not checked. LOW — OOM is rare.
- [x] **BadName**: `OpenFont` intentionally lenient (maps unknown fonts to "fixed" fallback) for AWT compatibility. `CloseFont` validates and sends BadFont (pre-existing).
- [x] **BadIDChoice**: `CreateWindow` validates XID ownership + uniqueness (pre-existing). `CreatePixmap`/`CreateGC`/`CreateCursor` are VOID requests — X11 spec forbids error replies on void requests, so getOrCreate/overwrite pattern is correct.

### 8.3 Reply Format Audit (MEDIUM) — COMPLETE (v1.19.29)
Ensure every reply-bearing request returns correctly formatted replies.

All 13 reply handlers audited. 12/13 were already correct; 1 bug found and fixed.

- [x] **GetWindowAttributes**: All 44 bytes correct — backing_store, visual, class, gravities, map_state, override_redirect, colormap, event masks, do_not_propagate_mask at correct offsets (WindowAttrOps.cpp:544).
- [x] **GetGeometry** (v1.19.29): **BUG FIXED** — depth was at bytes 22-23, moved to byte 1 per spec. Root, x, y, w, h, border_width all at correct offsets (ReplyWriter.cpp:59).
- [x] **QueryTree**: Root, parent, nchildren header correct. Variable-length children array with 4-byte alignment (QueryOps.cpp:428).
- [x] **GetProperty**: Format at byte 1, type/bytes_after/value_length at correct offsets. Delete flag handled (PropOps.cpp:475).
- [x] **QueryPointer**: same_screen at byte 1, root/child/coordinates/mask all correct (QueryOps.cpp:252).
- [x] **TranslateCoordinates**: same_screen at byte 1, child/dest_x/dest_y correct (QueryOps.cpp:756).
- [x] **GetImage**: Depth at byte 1, visual at bytes 8-11, pixel data properly aligned (DrawOps.cpp:388). XYPixmap not supported — ZPixmap only.
- [x] **QueryFont**: 60-byte struct with min/max bounds, properties array, 256 char infos (FontOps.cpp:168).
- [x] **ListFonts**: Pattern matching and name encoding correct.
- [x] **QueryColors**: nColors + 8-byte xrgb items correct (QueryOps.cpp:511).
- [x] **LookupColor**: Exact and screen RGB at correct offsets (ColorOps.cpp:460).
- [x] **GetKeyboardMapping**: keysymsPerKeycode at byte 1, keysym array correct (QueryOps.cpp:715).
- [x] **GetModifierMapping**: numKeyPerModifier at byte 1, modifier map correct (PointerOps.cpp:229).

### 8.4 Window Operations Audit (HIGH) — MOSTLY COMPLETE (v1.19.28)
Core window hierarchy ops are the most complex and most likely to have edge cases.

- [ ] **CreateWindow depth/visual validation**: InputOnly class not validated (depth 0, visual 0). LOW — no client we support uses InputOnly windows.
- [ ] **CreateWindow attribute inheritance**: ParentRelative background works. Full colormap/visual inheritance chain incomplete. LOW — toolkits manage this client-side.
- [x] **ConfigureWindow stacking** (pre-existing + v1.19.28): Above/Below/TopIf/BottomIf/Opposite all implemented. Sibling validation added — sends `BadMatch` if sibling doesn't share same parent.
- [x] **ConfigureWindow geometry constraints** (v1.19.28): Width/height == 0 now sends `BadValue`. Non-zero values clamped to uint16 range.
- [x] **MapWindow on already-mapped window** (pre-existing): Correctly checks `wasMapped`, skips MapNotify and UI push.
- [x] **UnmapWindow on already-unmapped window** (pre-existing): Correctly checks `wasMapped`, skips UnmapNotify.
- [x] **DestroyWindow recursive cleanup** (pre-existing): `descendantsOf` + reverse iteration for depth-first. DestroyNotify sent to child and parent. Resources freed via `erase()`.
- [x] **ReparentWindow correctness** (pre-existing + v1.19.28): Unmap/reparent/remap cycle correct. Added `BadMatch` if new parent is window itself or a descendant.
- [x] **CirculateWindow** (pre-existing): RaiseLowest and LowerHighest fully implemented with CirculateNotify events.
- [x] **ChangeSaveSet** (v1.19.28): Opcode 6 registered as no-op stub. Save-set only meaningful for reparenting WMs; safe no-op in rootless mode.

### 8.5 Graphics Operations Audit (MEDIUM)
Drawing ops correctness — clipping, coordinate handling, edge cases.

- [ ] **GC subwindow_mode**: `IncludeInferiors` vs `ClipByChildren` — currently ignored. Should affect clipping for all draw ops on windows.
- [ ] **CopyArea with overlapping src/dst**: Must handle correctly (copy direction matters to avoid corruption).
- [ ] **CopyArea GraphicsExposure**: When GC has `graphics_exposures=True` and source region is obscured, must send `GraphicsExposure` events for obscured sub-rects, followed by `NoExposure` if fully visible.
- [ ] **PutImage XYBitmap/XYPixmap**: Currently only ZPixmap fully supported. XYBitmap uses foreground/background. XYPixmap uses planemask. Verify bit order and padding.
- [ ] **GetImage XYPixmap**: Currently stub or ZPixmap-only. Need XYPixmap format support.
- [ ] **PolyLine join behavior**: `JoinMiter`, `JoinRound`, `JoinBevel` — currently not implemented (only square joins).
- [ ] **Line cap styles**: `CapNotLast`, `CapButt`, `CapRound`, `CapProjecting` — verify all are correct.
- [ ] **Dashed lines**: `SetDashes` stores dash list but verify `PolyLine`/`PolySegment` actually apply dash patterns.
- [ ] **Wide lines**: `line_width > 0` should produce thick lines. Currently may only draw 1px.
- [ ] **PolyArc angles**: Verify 1/64th degree units, counterclockwise direction, correct arc rendering for all quadrants.
- [ ] **FillPoly fill rule**: `EvenOddRule` vs `WindingRule` — verify both are correct for complex polygons.
- [ ] **GC plane_mask**: Verify plane_mask is applied to all drawing operations, not just shape ops.
- [ ] **GC function (ROP)**: Verify all 16 GC functions (GXclear through GXset) work correctly in all draw ops, not just shape ops.

### 8.6 Grab Semantics Audit (MEDIUM)
Pointer and keyboard grabs are complex and affect event routing globally.

- [ ] **GrabPointer event filtering**: Active grab should deliver only events matching `event_mask`. Events outside mask are discarded, not queued.
- [ ] **GrabPointer confine_to**: Pointer should be confined to the specified window. Currently not implemented.
- [ ] **GrabPointer cursor**: Grab cursor should replace the normal cursor during grab.
- [ ] **GrabButton passive grab activation**: Verify button/modifiers matching, `AnyButton`/`AnyModifier` wildcards, owner_events semantics.
- [ ] **GrabKey passive grab activation**: Same as GrabButton but for keyboard.
- [ ] **AllowEvents modes**: `AsyncPointer`, `SyncPointer`, `ReplayPointer`, `AsyncKeyboard`, `SyncKeyboard`, `ReplayKeyboard`, `AsyncBoth`, `SyncBoth` — currently stub. Need at least `AsyncPointer`/`AsyncKeyboard` for basic functionality.
- [ ] **GrabServer**: Should freeze all other clients' event processing. Currently a no-op stub.
- [ ] **Grab freezing**: `pointer_mode`/`keyboard_mode` Sync should freeze events until `AllowEvents`. Currently all grabs are async.
- [ ] **Automatic ungrab on window destroy**: Active grab on destroyed window should auto-ungrab.
- [ ] **Grab and focus interaction**: `GrabKeyboard` should send `FocusIn(mode=Grab)` / `FocusOut(mode=Grab)`. `UngrabKeyboard` should send `FocusIn(mode=Ungrab)` / `FocusOut(mode=Ungrab)`. Partially done (v1.15.17) — need full audit.

### 8.7 Atom and Property Audit (LOW)
Relatively simple but important for toolkit interop.

- [ ] **InternAtom only_if_exists**: When `only_if_exists=True` and atom doesn't exist, must reply with `atom=None` (not create it).
- [ ] **GetProperty delete semantics**: When `delete=True`, property should be deleted after read and `PropertyNotify(PropertyDelete)` sent. Verify partial reads with `long_offset`/`long_length` don't delete.
- [ ] **ChangeProperty modes**: `Replace`, `Prepend`, `Append` — verify all three modes work correctly with existing property data. Type and format must match for Prepend/Append (`BadMatch` if not).
- [ ] **RotateProperties**: Currently a void stub. Should rotate property values among the listed properties and send `PropertyNotify` for each.
- [ ] **ListProperties**: Verify returns all properties on the window, including pre-registered well-known atoms that have been set.

### 8.8 Selection Protocol Audit (MEDIUM)
Critical for clipboard and inter-client communication (already partially hardened in v1.15.18-22).

- [ ] **SetSelectionOwner timestamp validation**: If timestamp is earlier than last-set time, request should be ignored.
- [ ] **ConvertSelection with no owner**: Must send `SelectionNotify` with `property=None` to requestor.
- [ ] **MULTIPLE target**: Handle `MULTIPLE` target in `ConvertSelection` (multiple targets in single request).
- [ ] **INCR protocol**: Large property transfers via incremental protocol. Currently not implemented — large clipboard content will fail.
- [ ] **Selection timestamp**: `SelectionNotify` must include the timestamp from `ConvertSelection`, not current time.

### 8.9 Colormap Audit (LOW)
Minimal impact for TrueColor visuals but should be correct.

- [ ] **AllocColorCells / AllocColorPlanes**: Currently stubs. Should return `BadAlloc` for read-only colormaps (TrueColor).
- [ ] **FreeColors**: Verify plane_mask handling and pixel validation.
- [ ] **StoreColors / StoreNamedColor**: Should return `BadAccess` for read-only (TrueColor) colormaps.
- [ ] **CopyColormapAndFree**: Verify deep copy semantics.

### 8.10 Extension Protocol Audit (MEDIUM)
Review extension handlers for spec compliance.

- [ ] **RENDER format negotiation**: Verify `QueryPictFormats` returns formats matching the server's visual capabilities.
- [ ] **RENDER Composite clipping**: Verify clip rectangles, source/mask origins, and repeat modes.
- [ ] **RENDER glyph ops**: `CompositeGlyphs8/16/32` — verify glyph positioning, delta encoding, glyph set management.
- [ ] **SHAPE input shape**: Verify `ShapeInput` kind affects hit testing correctly.
- [ ] **RANDR reply sizes**: Verify all RANDR replies have correct padding and alignment for variable-length data.
- [ ] **XFIXES region ops**: Evaluate which XFIXES region operations are actually needed by target apps (Vivado, GTK apps) and implement those.
- [ ] **XInput2 event delivery**: Verify XI2 events have correct fields (deviceid, sourceid, time, valuators, modifiers, group).

### Testing Strategy
For each audit category, use a combination of:
1. **Code review**: Read the X11 protocol spec alongside the handler code
2. **xev validation**: Run `xev` and verify event fields match spec
3. **xdpyinfo / xprop / xwininfo**: Verify reply data
4. **x11perf**: Stress-test drawing ops at volume
5. **Vivado regression**: Run Vivado after each batch of fixes to ensure no regressions
6. **Protocol spec references**:
   - Core protocol: https://www.x.org/releases/current/doc/xproto/x11protocol.html
   - ICCCM: https://www.x.org/releases/current/doc/icccm/icccm.html
   - EWMH: https://specifications.freedesktop.org/wm-spec/latest/
   - RENDER: https://www.x.org/releases/current/doc/renderproto/renderproto.txt
   - SHAPE: https://www.x.org/releases/current/doc/shapeproto/shapeproto.txt
