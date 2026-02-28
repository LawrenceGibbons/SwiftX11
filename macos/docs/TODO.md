# SwiftX11 TODO

Last updated: 2026-02-28

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

### Input / Events
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
- [x] Text: ImageText8, PolyText8 via BDF glyph bitmaps
- [x] Cursor management: CreateCursor, CreateGlyphCursor, FreeCursor, cursor shape application

### Debug / Instrumentation
- [x] Two-tier trace system: #ifndef NDEBUG lifecycle traces + #ifdef X11_TRACE_VERBOSE per-op traces
- [x] Version banner: SwiftX11 v{version} at startup

---

## Phase 1: Core Protocol Gaps (Required for Vivado/Vitis)

Vivado uses Java Swing (renders client-side via Java 2D, uploads via PutImage). Vitis uses Eclipse SWT backed by GTK/GDK/Cairo (renders client-side, uploads via CopyArea/PutImage). Both need solid window management, events, properties, and selections.

### 1.1 GC Semantics (HIGH — affects all drawing)
GC function, planemask, and clipping are used by every toolkit.
- [ ] **GC function (GXcopy, GXxor, etc.)**: Apply in all draw paths (PolyFillRectangle, CopyArea, text ops, etc.). Currently everything is GXcopy-only.
- [ ] **GC planemask**: Apply in all draw paths (currently ignored).
- [ ] **SetClipRectangles (opcode 59)**: Implement GC clip region. Used heavily by toolkits to restrict drawing to widget bounds.
- [ ] **SetDashes (opcode 58)**: Implement dash pattern for line drawing. Used for selection rectangles, focus indicators.
- [ ] **GC clip enforcement**: All draw ops must check GC clip rect and skip/clamp pixels outside it.
- [ ] **GC fill-style**: Support Solid, Tiled, OpaqueStippled, Stippled fills.
- [ ] **GC tile/stipple**: Store tile/stipple pixmap in GC, apply during fills.

### 1.2 Missing Opcodes (HIGH — crash prevention)
Unhandled opcodes log warnings but don't send replies, causing XCB sequence desync for reply-bearing requests. Java/GTK may use any of these.
- [ ] **ReparentWindow (opcode 7)**: GTK reparents widgets internally. Must update parent chain in WindowTable and adjust geometry.
- [ ] **ChangeActivePointerGrab (opcode 30)**: Modify event mask during active grab. Used by GTK drag-and-drop.
- [ ] **QueryKeymap (opcode 44)**: Returns 32-byte keymap vector. Java checks this. Return all-zeros as stub.
- [ ] **GetMotionEvents (opcode 39)**: Returns motion history. Return empty list as stub (reply required).
- [ ] **SetFontPath (opcode 51)**: Accept and ignore (void, no reply needed).
- [ ] **GetFontPath (opcode 52)**: Return empty font path list (reply required).
- [ ] **DestroySubwindows (opcode 5)**: Destroy all children of a window.
- [ ] **RotateProperties (opcode 114)**: Rotate property list. Accept and process or stub.

### 1.3 16-bit Text (MEDIUM — Unicode support)
Java/GTK use 16-bit text for internationalized strings.
- [ ] **PolyText16 (opcode 75)**: Draw 16-bit character strings. Map to existing BDF fonts (high byte selects font page).
- [ ] **ImageText16 (opcode 77)**: Draw 16-bit text with opaque background.
- [ ] **QueryTextExtents**: Verify 16-bit character extent calculations work.

### 1.4 Selections / Clipboard (MEDIUM — copy/paste)
Vivado/Vitis need clipboard for copy/paste between X11 apps and potentially with macOS.
- [ ] **Selection request/notify flow**: Verify ConvertSelection/SelectionNotify works end-to-end between X11 clients.
- [ ] **CLIPBOARD atom**: Register and handle CLIPBOARD in addition to PRIMARY.
- [ ] **TARGETS**: Support TARGETS conversion (advertise available data types).
- [ ] **macOS clipboard bridge**: Bridge NSPasteboard <-> X11 CLIPBOARD selection for cross-environment copy/paste.

### 1.5 WarpPointer (MEDIUM — stub upgrade)
- [ ] **WarpPointer (opcode 41)**: Currently accepted but pointer not moved. Java tooltips and dialogs may use this. Implement via CGWarpMouseCursorPosition.

---

## Phase 2: X11 Extensions (Required for Modern Toolkits)

Java 2D, GTK/Cairo, and Pango all query for extensions. SwiftX11 currently returns "not present" for all extensions (QueryExtension replies present=0). This works but forces fallback paths that may be slower or miss features.

### 2.1 RENDER Extension (HIGH — anti-aliased fonts, alpha compositing)
The single most impactful extension. Java 2D's XRender pipeline, GTK/Cairo's rendering, and Pango's font rendering all use RENDER. Without it, clients fall back to core protocol (bitmap fonts, no alpha blending).
- [ ] **QueryExtension("RENDER")**: Return present=1, major opcode for RENDER.
- [ ] **RenderQueryVersion**: Negotiate version (0.11 is widely supported).
- [ ] **RenderQueryPictFormats**: Return available picture formats (ARGB32, RGB24, A8, A1).
- [ ] **CreatePicture / FreePicture**: Associate a Picture with a Drawable + PictFormat.
- [ ] **Composite**: The core compositing operation. Combine src+mask -> dst with alpha blending. This is what Cairo uses for everything.
- [ ] **FillRectangles**: Fill rectangles with a color on a Picture (used for solid fills with alpha).
- [ ] **Trapezoids / Triangles**: Geometric fill operations (used by Cairo for vector paths).
- [ ] **AddGlyphs / CompositeGlyphs8/16/32**: Server-side glyph caching + rendering. Pango/Cairo upload font glyphs once, then reference by ID for fast text drawing.
- [ ] **CreateGlyphSet / FreeGlyphSet**: Glyph set management.
- [ ] **SetPictureClipRectangles**: Clip region on Pictures.

### 2.2 BIG-REQUESTS Extension (HIGH — large images)
Vivado schematics and waveform views can be large. Without BIG-REQUESTS, maximum request size is 262140 bytes (~256KB), limiting PutImage to ~256KB per call.
- [ ] **QueryExtension("BIG-REQUESTS")**: Return present=1.
- [ ] **BigReqEnable**: Return maximum request size (e.g., 16MB). Changes wire format: 4-byte length field becomes 8-byte for oversized requests.
- [ ] **Wire format**: Detect extended-length requests (length==0 in header → read 4-byte extended length).

### 2.3 XFIXES Extension (MEDIUM — cursor, regions)
GTK and Java use XFIXES for cursor visibility and region operations.
- [ ] **QueryExtension("XFIXES")**: Return present=1.
- [ ] **XFixesQueryVersion**: Negotiate version.
- [ ] **XFixesShowCursor / HideCursor**: Cursor visibility control.
- [ ] **XFixesCreateRegion / SetWindowShapeRegion**: Region operations.
- [ ] **XFixesSelectCursorInput / GetCursorImage**: Cursor change notification.

### 2.4 SHAPE Extension (LOW — non-rectangular windows)
Some splash screens and tooltips use shaped windows.
- [ ] **QueryExtension("SHAPE")**: Return present=1.
- [ ] **ShapeRectangles / ShapeMask**: Define non-rectangular window shape.
- [ ] **ShapeQueryExtents**: Query window shape.

### 2.5 Other Extensions (LOW — query but don't need full impl)
These are frequently queried. Return present=0 with correct reply format, or minimal stubs:
- [ ] **MIT-SHM**: Shared memory (not applicable over network/container — present=0 is correct).
- [ ] **RANDR**: Screen configuration (single-screen stub).
- [ ] **Xinerama**: Multi-monitor (single-screen stub).
- [ ] **XInput / XInput2**: Extended input (present=0 is fine initially).
- [ ] **DPMS**: Display power management (present=0).
- [ ] **SYNC**: Synchronization (present=0).
- [ ] **Generic Event Extension (GE)**: Required by XInput2 (present=0 if XI2 not implemented).

---

## Phase 3: Font Infrastructure (Required for Readable UI)

Vivado and Vitis need proper fonts. The current BDF-only system works for xterm but won't satisfy GTK/Java apps that expect a richer font ecosystem.

### 3.1 Font Matching and XLFD (HIGH)
- [ ] **Wildcard XLFD matching**: ListFonts with wildcards like `-*-helvetica-*-*-*-*-12-*-*-*-*-*-*-*` must match correctly. Currently may not handle all wildcard positions.
- [ ] **Font aliases**: Support standard alias files (e.g., "fixed" -> specific XLFD).
- [ ] **Scaled fonts**: Return synthetic font names for requested pixel sizes (scale BDF glyphs or report closest available).

### 3.2 Additional BDF Fonts (HIGH)
- [ ] **Bundle standard X11 fonts**: misc/fixed, 100dpi, 75dpi collections (cursor, helvetica, times, courier).
- [ ] **Font directory scanning**: Load all .bdf files from configured directories at startup.
- [ ] **PCF font support**: Read PCF (Portable Compiled Font) format — more compact than BDF, same data.

### 3.3 TrueType / CoreText Integration (MEDIUM — for RENDER extension)
If RENDER is implemented, client-side font rendering (Pango/FreeType) becomes the primary path. Server-side fonts become less critical.
- [ ] **Xft/fontconfig on client side**: Clients use their own FreeType + fontconfig to render glyphs, upload via RENDER CompositeGlyphs. Server just needs RENDER support.
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
- [ ] **ConfigureWindow stack mode**: Handle Above/Below/TopIf/BottomIf/Opposite sibling stacking.
- [ ] **ConfigureWindow border width**: Track and report (even if always 0).
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

### 5.4 ICCCM / Window Manager Compliance (LOW)
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

# Phase 2+ (RENDER)
# rendercheck               # run when RENDER is implemented
# gtk3-demo &               # run when GTK works
```

---

## Priority Order for Vivado/Vitis

1. **GC clipping (SetClipRectangles)** — Without this, drawing bleeds outside widget bounds
2. **Missing reply-bearing opcodes (QueryKeymap, GetMotionEvents, GetFontPath)** — Prevent XCB sequence crashes
3. **ReparentWindow** — GTK reparents widgets internally
4. **BIG-REQUESTS extension** — Large schematics/waveforms exceed 256KB request limit
5. **Error handling** — Bad replies/missing errors confuse toolkits
6. **RENDER extension** — Anti-aliased fonts make UI usable (without: bitmap fonts only)
7. **Container networking** — TCP + Unix socket + xauth for Docker workflow
8. **16-bit text** — Unicode labels in Vivado UI
9. **Selections/clipboard** — Copy/paste between apps
10. **Font infrastructure** — Broader font matching for toolkit defaults
