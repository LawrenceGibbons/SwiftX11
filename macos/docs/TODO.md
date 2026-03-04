# SwiftX11 TODO

Last updated: 2026-03-04 (v1.7.3 — monotonic wire-sequence floor + resize border fix)

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

Java 2D, GTK/Cairo, and Pango all query for extensions. **Current state (v1.7.3)**: **BIG-REQUESTS**, **RENDER**, and **XFIXES** are advertised as present. All RENDER operations fully implemented including Trapezoids, Triangles, gradient sources. **SHAPE** has full stub support but is NOT advertised (causes xeyes to take a broken oval-window code path — needs actual shape clipping first). Handler code also exists for RANDR, Xinerama, and GE (NOT advertised yet).

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

### 2.4 SHAPE Extension (LOW — non-rectangular windows) — stubs complete, NOT advertised
Stub handlers exist for all core SHAPE operations, but SHAPE is deliberately NOT advertised because xeyes uses SHAPE for oval window clipping. Since our implementation silently consumes shape ops without actual pixel clipping, xeyes renders solid-black ovals instead of transparent eye shapes. SHAPE can be advertised once actual shape-based clipping is implemented (or if xeyes is not a priority).
- [x] **QueryExtension("SHAPE")**: Handler returns present=1 internally, but NOT in QueryExtension/ListExtensions responses (v1.6.0).
- [x] **ShapeQueryVersion**: Returns version 1.1 (v1.4.0).
- [x] **ShapeRectangles (sub-opcode 1)**: Consume silently — all windows remain rectangular (v1.6.0).
- [x] **ShapeMask (sub-opcode 2)**: Consume silently (v1.6.0).
- [x] **ShapeQueryExtents (sub-opcode 5)**: Returns window bounding rect (v1.6.0).
- [ ] **Actual shape clipping**: Implement pixel-level clipping based on stored shape regions. Required before advertising SHAPE.

### 2.5 Other Extensions (LOW — query but don't need full impl)
These are frequently queried. Return present=0 with correct reply format, or minimal stubs:
- [ ] **MIT-SHM**: Shared memory (not applicable over network/container — present=0 is correct).
- [x] **RANDR**: Handler code returns present=1, major 136, RRQueryVersion returns 1.5, single-screen stub — but **NOT advertised** yet (v1.4.0).
- [x] **Xinerama**: Handler code returns present=1, major 137, XineramaIsActive=1, QueryScreens returns 1 screen 1920×1080 — but **NOT advertised** yet (v1.4.0).
- [ ] **XInput / XInput2**: Extended input (present=0 is fine initially).
- [ ] **DPMS**: Display power management (present=0).
- [ ] **SYNC**: Synchronization (present=0).
- [x] **Generic Event Extension (GE)**: Handler code returns present=1, major 138, GEQueryVersion returns 1.0 — but **NOT advertised** yet (v1.4.0).

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

### 3.3 TrueType / CoreText Integration (MEDIUM — deferred)
Client-side font rendering via RENDER CompositeGlyphs (Pango/FreeType) is the primary path for modern toolkits. Server-side PCF/BDF fonts cover legacy apps (xterm, xcalc, xclock).
- [ ] **Xft/fontconfig on client side**: Clients use their own FreeType + fontconfig to render glyphs, upload via RENDER CompositeGlyphs. Server just needs RENDER support (done).
- [ ] **CoreText bridge (optional)**: Map X11 font requests to macOS system fonts via CoreText for high-quality server-side rendering.

---

## Phase 4: Robustness and Correctness

### 4.1 Error Handling (HIGH)
- [ ] **Proper X11 error generation**: BadWindow, BadDrawable, BadGC, BadMatch, BadValue, BadAtom, BadPixmap, BadFont, BadAccess, BadAlloc for all relevant requests.
- [ ] **Error reply format**: Ensure error replies have correct seq, minor opcode, major opcode fields.
- [ ] **Error for destroyed resources**: Requests referencing destroyed XIDs should generate BadWindow/BadDrawable instead of silently failing.

### 4.2 Wire Protocol Correctness (MEDIUM)
- [ ] **Big-endian clients**: ByteReader/ReplyWriter currently assume little-endian. Java may connect with big-endian byte order. Need to respect client byte order from setup.
- [ ] **Padding verification**: Ensure all replies are padded to 4-byte boundaries.
- [ ] **Request length validation**: Verify request body length matches expected size for each opcode.

### 4.3 Window Management Correctness (MEDIUM)
- [x] **ConfigureWindow stack mode**: Above/Below/TopIf/BottomIf/Opposite sibling stacking with CWSibling support (v1.5.6).
- [x] **ConfigureWindow border width**: Tracked, stored, and rendered (v1.3.0).
- [ ] **Override-redirect**: Honor override_redirect attribute (don't apply WM decoration/placement).
- [ ] **Gravity**: Implement win_gravity and bit_gravity for resize behavior.
- [ ] **Backing store**: Accept BackingStore attribute (can be NotUseful stub).

### 4.4 Colormap (LOW — TrueColor is sufficient)
SwiftX11 advertises TrueColor visual. Vivado/Vitis Java apps use TrueColor. Current colormap stubs (accept requests, return reasonable defaults) should work.
- [ ] **Verify TrueColor visual advertisement**: Ensure depth=24, class=TrueColor, red/green/blue masks correct in setup reply.
- [ ] **AllocColor correctness**: For TrueColor, AllocColor should return the closest matching pixel value (currently returns the RGB packed into pixel — verify this is correct).

---

## Phase 5: Performance and Polish

### 5.1 Rendering Performance (MEDIUM)
- [ ] **Software present path**: Implement partial CGImage blit (currently full surface copy).
- [ ] **PutImage optimization**: Large PutImage calls (Vivado waveform/schematic) need efficient copy paths.
- [ ] **Expose coalescing**: Batch expose events to reduce client redraw overhead.

### 5.2 Container / Network Support (HIGH for Vivado use case)
- [ ] **TCP socket listener**: Verify TCP connections work from Docker container (DISPLAY=host.docker.internal:0).
- [ ] **Unix socket**: Add /tmp/.X11-unix/X0 Unix domain socket support for local containers.
- [ ] **Xauth**: Basic MIT-MAGIC-COOKIE-1 authentication (or xhost + for development).
- [ ] **Latency tolerance**: Ensure protocol handling doesn't assume local-only latency.

### 5.3 Keyboard (MEDIUM)
- [ ] **Full keysym mapping**: Map macOS virtual keycodes to X11 keysyms comprehensively (currently minimal).
- [ ] **Modifier mapping**: GetModifierMapping should return a mapping that matches macOS keyboard layout.
- [ ] **Keymap state**: QueryKeymap returns current key state (currently not implemented).
- [ ] **XKB (optional)**: Modern clients may query for XKB extension — return not-present is acceptable.

### 5.4 Window Close / Client Lifecycle (HIGH — user experience)
- [ ] **Window close kills client**: When user clicks the red close button (Cocoa windowWillClose), the X11 client should be terminated. Two approaches:
  - **WM_DELETE_WINDOW** (ICCCM-compliant): If WM_PROTOCOLS includes WM_DELETE_WINDOW, send a ClientMessage event. Well-behaved clients (xterm, xcalc) will exit gracefully.
  - **Forceful disconnect**: If client doesn't support WM_DELETE_WINDOW (or as fallback), close the client socket (fd) to force disconnect. The server's eraseOwnedBy() cleanup handles resource teardown.
- [ ] **Keyboard shortcut**: Cmd+W should also trigger window close with the same behavior.

### 5.5 Window Menu Integration (MEDIUM — user experience)
- [ ] **Window menu entries**: Each X11 client window should appear as an entry in SwiftX11's macOS "Window" menu. Selecting the entry should bring the window to the front and give it focus (makeKeyAndOrderFront).
- [ ] **Dynamic updates**: Entries added on MapWindow, removed on DestroyWindow/UnmapWindow. Use WM_NAME property (or fallback to XID) as the menu item title.

### 5.6 ICCCM / Window Manager Compliance (LOW)
- [ ] **WM_HINTS**: Read and honor WM_HINTS property (icon, initial state, input model).
- [ ] **WM_NORMAL_HINTS**: Read and honor size hints (min/max/increment size, aspect ratio).
- [ ] **WM_PROTOCOLS**: Support WM_DELETE_WINDOW (send ClientMessage instead of destroying).
- [ ] **_NET_WM_* (EWMH)**: Basic Extended Window Manager Hints support.

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

### Remaining priorities
1. **Complete Phase 2.4** — SHAPE actual clipping: implement pixel-level clipping from stored shape regions, then advertise
2. **Complete Phase 2.5** — Enable remaining extensions: advertise RANDR, Xinerama, GE one at a time (handler stubs already exist)
3. **Complete Phase 3.3** — Verify Xft/fontconfig client-side rendering works via RENDER CompositeGlyphs; optional CoreText bridge
4. **Window close → client kill** — Red button should terminate X11 client (WM_DELETE_WINDOW or socket close) (Phase 5.4)
5. **Error handling** — Bad replies/missing errors confuse toolkits (Phase 4)
6. **Container networking** — TCP + Unix socket + xauth for Docker workflow (Phase 5.2)

### Phase 2+3 Status Assessment (v1.7.3)
**Phases 2 and 3 are mostly complete, with remaining items in 2.4, 2.5, and 3.3.** All RENDER operations are implemented: Composite (with mask + gradient sources), CompositeGlyphs8/16/32, Trapezoids, Triangles/TriStrip/TriFan, FillRectangles, all Porter-Duff blend modes, gradient source pictures (Linear/Radial/Conical). Extensions: BIG-REQUESTS, RENDER, and XFIXES are advertised and functional. SHAPE has stub support but is held back to avoid breaking xeyes — actual shape clipping (2.4) is still needed. Remaining extensions (2.5): RANDR, Xinerama, GE have handler stubs but are not yet advertised. Fonts: XLFD wildcard matching, PCF font loading, Symbol encoding, ListFontsWithInfo all work. 3.3 (TrueType/CoreText) has unchecked items (client-side Xft/fontconfig verification, optional CoreText bridge).

**v1.7.2 adds a reply-tracking safety net** that prevents XCB sequence desync crashes (the xcalc resize crash). Missing replies for reply-bearing opcodes now automatically get a BadImplementation error response.

**v1.7.3 adds a monotonic wire-sequence floor** that prevents event sequence regression when drainHostCommands() interleaves with readAndDispatch(). Also fixes button borders disappearing after window resize.

**Next priorities**: Complete 2.4, 2.5, and 3.3 before starting Phase 4 (window lifecycle, error handling).

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

### Bug fixes (v1.7.3)
- **xcalc resize crash — monotonic wire-sequence floor**: Even with v1.7.2's reply-tracking safety net, xcalc resize still triggered `[xcb] Unknown sequence number` crashes. Root cause: `drainHostCommands()` interleaves with `readAndDispatch()`, and host commands (resize → ConfigureNotify, Expose) carry stale sequences from `lastSeq()` that are behind sequences already sent during client request dispatch. XCB widens 16-bit sequences to 64-bit monotonically — any backwards sequence causes crash. Fixed by tracking `max_wire_seq_` (highest sequence ever sent) in `XProtoTransport::sendAll()` and bumping any stale sequence forward. Payload-aware: `payload_remaining_` counter distinguishes reply payload chunks (bytes[2:3] are arbitrary data) from response headers (bytes[2:3] are sequence numbers).
- **Button borders missing after resize**: After window resize, button outlines (server-drawn borders) disappeared — only text labels rendered. Root cause: `ExposeChildren` handler was "non-destructive" (only sent Expose events), but after resize the surface is cleared to white, so server-drawn borders and backgrounds were lost. Fixed by adding `fillWindowBorderIfReady()` and `fillWindowBackgroundIfReady()` calls in `ExposeChildren`, matching `sendExposeSubtree` behavior.

### Bug fixes (v1.7.2)
- **xcalc resize crash (XCB sequence desync)**: Resizing xcalc triggered `[xcb] Unknown sequence number while processing queue` → abort. Root cause: a reply-bearing request was dispatched to a handler that didn't send a reply, causing XCB's internal sequence tracking to desync. Fixed with three-layer defense:
  1. **Reply-sent tracking**: `XProtoTransport::reply_sent_` flag set in `sendAll()` when sending a reply (byte[0]==1) or error (byte[0]==0). Reset before each dispatch.
  2. **Core opcode safety net**: `isReplyBearingCore()` 128-entry static table in `XProtoServer::dispatch()`. After dispatch, if reply-bearing and no reply sent, sends `BadImplementation` error. Covers: unregistered handlers, handler exceptions, and normal dispatch without reply.
  3. **Extension error replies**: All extension `default` switch cases (XFIXES, SHAPE, RANDR, Xinerama, GE, RENDER) now send `BadRequest` error instead of silently consuming bytes.
