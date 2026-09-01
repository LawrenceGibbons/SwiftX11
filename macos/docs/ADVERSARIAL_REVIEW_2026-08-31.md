# SwiftX11 Adversarial Protocol Review — 2026-08-31

**Scope:** X11 protocol core (`X11LowLevel/cpp/X11Protocol/`) and the Swift window/input layer, reviewed against the core X11 protocol, ICCCM/EWMH, and the RENDER/SHAPE/XFIXES/RANDR/XI2/XTEST/Composite/DAMAGE extension specs. Six independent review passes: (1) wire protocol & sequence integrity, (2) event/input/grab/focus semantics, (3) WM emulation & dialog sizing, (4) RENDER/SHAPE/core drawing, (5) extension advertisement honesty, (6) resource lifecycle/properties/selections/fonts.

Findings are marked **CONFIRMED** (reviewer read the cited code) or **PLAUSIBLE** (mechanism inferred, needs verification). File:line references are relative to `X11LowLevel/cpp/X11Protocol/` unless prefixed with `SwiftX11/`. Line numbers are as of v1.19.36.

---

## Executive summary

The codebase is far more real than a typical "X11 shim" — RENDER's rasterizer, the atom table, WM_NORMAL_HINTS parsing, Xinerama/RANDR geometry, cross-client sequence restamping, and the surface-lifetime redesign all check out. But the review found six systemic shortcut patterns that explain most of the recurring mystery bugs, plus one urgent doc-vs-code discrepancy:

1. **⚠️ The XTEST v2.0 downgrade is NOT in the code.** `handleXTEST` GetVersion still replies **2.2** (`src/Ops/ExtensionOps.cpp:1599-1610` — `rep[1]=2`, minor=2). CLAUDE.md/HANDOFF.md claim v1.19.14 downgraded to 2.0 as the workaround for the Vivado GrabControl crash. Either the downgrade was lost in a merge or the docs are wrong — but as shipped, the GrabControl crash trigger is live again. Verify immediately.

2. **The sequence "floor" is the disease, not the cure.** A real X server never rewrites sequence numbers. The monotonic wire-seq floor, the SEQ_WRAP heuristic, and the payload-remaining counter form a rewrite engine that (a) breaks XCB's reply matching whenever it fires on a reply, (b) can be *poisoned* by a single mis-stamped packet and then misclassify recovery as a "16-bit wrap" — which matches the post-sleep crash log signature exactly — and (c) was partly built to satisfy a **broken diagnostic** that misreads reply payload chunks as packet headers. Three real stamping bugs feed it (see §1). Fix those, then delete the floor.

3. **XI2 was condemned on the wrong evidence.** The GenericEvent framing/lengths are **correct** (verified against xXIDeviceEvent/xXIEnterEvent/xXIRawEvent). The real bug: XI2 selection is stored in a server-global mask and events route by *window owner*, so one client's `XISelectEvents(root)` floods **other clients** with events they never selected, while the selecting client starves. That, plus the phantom [SEQ_REGRESS] diagnostics, is the likely "XI2 wire corruption." There is a concrete path to re-enabling XI2 (§5).

4. **The single-owner event-delivery model is a load-bearing shortcut.** Windows have one `event_mask` and one `owner_fd`. Three independent passes hit this wall: XI2 root selection (above), PropertyNotify going only to the window owner (spec: all clients selecting PropertyChangeMask — and a second client's ChangeWindowAttributes *clobbers* the owner's mask), and SendEvent ignoring its event-mask destination semantics. Per-(window, client) mask tracking is the single highest-leverage architectural fix on this list.

5. **"Silently consume" is a mystery-bug factory.** Advertised-but-mute: DAMAGE never sends DamageNotify (clients hang forever), Composite's NameWindowPixmap discards the client's pixmap XID (delayed BadPixmap "out of nowhere"), XFIXES 5.0 has no regions and never sends XFixesSelectionNotify, XTEST FakeInput does nothing (Robot/xdotool hang), RENDER ignores SetPictureTransform/SetPictureFilter/CreateCursor. Silent-success on requests with observable contracts is strictly worse than not advertising: the failure surfaces requests later in an unrelated subsystem.

6. **The small-dialog bug is not one bug — six confirmed mechanisms remain** (§3), the worst being: the WM_NORMAL_HINTS handler **resizing already-mapped windows** with no ConfigureNotify; a one-shot `surface_resize_exposed` flag that drops the client's paint on the *second* surface reallocation with no re-Expose; and a Cocoa resize-echo path that can walk a dialog's X11 geometry back to a stale intermediate size that Swing then adopts.

7. **Long sessions decay.** rid_base wraps at 256 client connections (connection #257 collides with connection #1 — the long-lived Vivado JVM); pixmaps/GCs/cursors/fonts are never freed on disconnect (`FontTable::eraseOwnedBy` exists with zero callers); dead selection owners are never cleared (paste then hangs); destroyed windows leave properties, peak-size entries, and keyboard grabs behind for recycled XIDs to inherit.

---

## 1. Wire protocol & sequence integrity

The output path is single-threaded (all sends funnel through the daemon poll thread; `sendAll`'s thread guard *silently drops* wrong-thread sends — itself worth an assert). The corruption mechanisms are in-stream, not cross-thread:

| # | Finding | Sev | Where |
|---|---|---|---|
| 1.1 | **Floor rewrites reply/error sequences.** Any packet ≥32B not mid-payload whose bytes[2:3] "regress" vs `max_wire_seq_` gets rewritten. XCB matches replies to requests by this exact number; a bumped reply orphans the waiter → hang or "Unknown sequence number" abort. CONFIRMED | Critical | `src/Transport/XProtoTransport.cpp:202-334` |
| 1.2 | **SEQ_WRAP heuristic = the post-sleep death signature.** On regression, if `(int16_t)(last_request_seq_ - max_wire_seq_) < 0` it declares "wrap", resets the floor, and sends the packet with its **original regressing seq**. For replies, `pkt_seq == last_request_seq_` by construction, so a *poisoned* floor (one packet that ever carried too-large bytes[2:3]) always takes this branch: client widens +65536, next responses "regress" again, XCB dies. The `[SEQ_WRAP]` immediately before the post-sleep disconnect is precisely this signature, not a real wrap. CONFIRMED mechanism; post-sleep link PLAUSIBLE — log `max_wire_seq_` vs `last_request_seq_` at SEQ_WRAP to confirm. | Critical | `src/Transport/XProtoTransport.cpp:241-266` |
| 1.3 | **WindowClose sends UnmapNotify/DestroyNotify with seq=0.** The only remaining zero-seq emitter (grep-verified). Normally masked by the floor; on a fresh transport or during the seq-0 blind spot it goes out raw → poisons the client (+64K widen). Closing a Vivado popup at the wrong moment suffices. CONFIRMED | High | `src/Transport/XProtoDaemon.cpp:929-945` |
| 1.4 | **The [SEQ_REGRESS] detector and wire ring buffer misread payload as headers.** The debug regression detector fires on any 32-byte buffer *before* the payload check (GetImage rows are exactly 32B for 8px-wide reads); `recordWirePacket` is called after `payload_remaining_` is decremented so every payload's final chunk is recorded as a phantom packet. **The crash dumps used to condemn XI2 contain fabricated packets.** CONFIRMED | High | `src/Transport/XProtoTransport.cpp:138-186, 336-341, 791-795` |
| 1.5 | **No release-mode defense against header-lenw/payload mismatch.** All under/over-send checks compile out in release; a mismatch desyncs the client AND lets line 324 set the floor from arbitrary payload bytes (arming 1.2). Hot reply paths currently audited consistent, but nothing enforces it; XIQueryPointer calls `sendAll` directly bypassing even debug checks. Fix structurally: one `writeReply(header, payload)` entry point owning lenw. CONFIRMED | High | `src/Transport/XProtoTransport.cpp:217-334, 413-585` |
| 1.6 | **BIG-REQUESTS length unbounded → remote kill of the whole app.** `ext_words` validated only `>= 2`; up to ~17GB `resize()` runs *outside* dispatch's try/catch → `std::terminate` on the daemon thread. One flipped bit in a length field kills every session. Core-request length caps and ByteReader bounds checks are otherwise solid. CONFIRMED | High | `src/Transport/XProtoDaemon.cpp:658-688` |
| 1.7 | **Reply safety net covers core opcodes only.** A ByteReader throw mid-handler on a malformed reply-bearing *extension* request (major ≥128) escapes to dispatch's catch and sends nothing → the exact XCB hang the net exists to prevent. Also `requestHasReply()` disagrees with `isReplyBearingCore()` (39/44/52). CONFIRMED | Medium | `src/Core/XProtoServer.cpp:221-317`, `src/Ops/ReplyWriter.cpp:15-57` |
| 1.8 | **Head-of-line blocking / starvation.** One client is drained to `NeedMore` with no request budget; `sendAll`'s EAGAIN loop blocks the whole poll thread on one wedged client (and the floor's combined-send path busy-spins with no `poll()`). Matches menu-freeze/backpressure symptoms. CONFIRMED | Medium | `src/Transport/XProtoDaemon.cpp:337-360`, `XProtoTransport.cpp:363-405` |
| 1.9 | Verified GOOD: cross-client restamp (`sendEventCrossClient` stamps target's `lastSeq()` — correct per spec, keep it); `nextEventSeq()` is dead code (delete before someone uses it); 'B' byte-order clients cleanly rejected (nit: failure reply length is LE). | — | `src/Transport/XProtoDaemon.cpp:202-241` |

**Remediation story (ordered):** fix 1.3 and the XI2 routing (§5), extend the safety net (1.7), clamp 1.6 — then **delete** the floor, the wrap heuristic, and the payload heuristic in favor of a structured reply writer, and fix the instrumentation (1.4) so the wire can be observed honestly. Until the floor is gone, every new stamping bug will be silently converted into a delayed, unattributable desync.

---

## 2. Event, input, grab, and focus semantics

| # | Finding | Sev | Where |
|---|---|---|---|
| 2.1 | **QueryPointer returns a garbage `mask`**: `buttons \| mods` raw internal bits, never `toX11State()`. Button1-held reports as ShiftMask with no buttons. Java polls XQueryPointer in drag loops / `MouseInfo` — it concludes no button is down mid-drag. One-line fix (both reply paths incl. the `host==0` early return). CONFIRMED | Critical | `src/Ops/QueryOps.cpp:271, 302` |
| 2.2 | **GrabKeyboard never routes keys.** Handler stores the grab and sends focus events, but the Key host-command path routes purely by focus and never consults the grab table (no getter even exists besides `clearKeyboardGrab`). Swing popup menus/combos grab the keyboard for arrow/Escape navigation — keys go to the frame instead. CONFIRMED | Critical | `src/Ops/GrabOps.cpp:207-253`, `src/XProtoServerBridge.cpp:985-1056`, `include/Core/GrabTable.hpp` |
| 2.3 | **EnterNotify/LeaveNotify byte 31 wrong**: writes 1 ("sameScreen") into the packed flags byte where bit0=focus, bit1=same_screen → every crossing event says focus=True, same_screen=False. Toolkits taking the "different screen" path ignore coordinates; AWT reads `.focus` for containment bookkeeping. Fix: `ev[31] = 0x02 \| focusBit`. (The XI2 crossing sender gets this right.) CONFIRMED | Critical | `src/Ops/EventOps.cpp:573-575` |
| 2.4 | **GrabModeSync is silently async; AllowEvents is a no-op** (ReplayPointer included). No freeze semantics anywhere. Clients using sync grabs get an unfrozen firehose; ReplayPointer-based click-through logic breaks. CONFIRMED | High | `src/Ops/GrabOps.cpp:79-80, 302-304` |
| 2.5 | **No grab timestamps, no ownership, no AlreadyGrabbed.** Grabs unconditionally overwrite each other; any client's `XUngrabPointer(CurrentTime)` (Java does this liberally) destroys another client's active grab mid-menu/mid-drag. Add `client_fd` + `grab_time` to `PointerGrab` (also the planned `owner_fd` item), validate time, return status codes. CONFIRMED | High | `src/Ops/GrabOps.cpp:84, 97-103`, `src/Core/GrabTable.cpp:65-76` |
| 2.6 | **Implicit button-grab emulation uses the wrong window and skips mask filtering.** `drag_xid` is set to the pre-propagation pick target, but the spec's automatic grab belongs to the window the press was *delivered* to; during drag, motion goes to `drag_xid` unconditionally (no mask check) while the ancestor that selected ButtonMotionMask gets nothing. Fix: `drag_xid = deliver`; filter with the existing `grabWantsMotion()`. CONFIRMED | High | `src/XProtoServerBridge.cpp:789, 820-835`, `src/XProtoNotifyBridge.cpp:260-267` |
| 2.7 | **Crossing detail/Virtual chain absent** (detail always NotifyAncestor; no intermediate Virtual events; no mode=Grab/Ungrab crossings). Xt/Motif and AWT use detail to distinguish "left for my child" from "truly left" → stuck/flickering hover highlights. CONFIRMED | High | `src/Ops/EventOps.cpp:550`, `src/XProtoNotifyBridge.cpp:153-172` |
| 2.8 | **do_not_propagate_mask parsed-but-discarded**; propagation walks climb unconditionally. AWT sets CWDontPropagate on every window to fence off ancestors → duplicate/misattributed MouseEvents. GetWindowAttributes also lies (reports 0). CONFIRMED | High | `src/Ops/WindowAttrOps.cpp:201` + button/key/motion walks |
| 2.9 | **SetInputFocus: revert-to and time ignored; focus resets to None on destroy/unmap** instead of reverting to Parent/PointerRoot; GetInputFocus always claims revertTo=None. Classic "keyboard dead after closing a dialog until user clicks". The WM_TAKE_FOCUS "bounce" hack treats a symptom this would fix properly. CONFIRMED | High | `src/Ops/QueryOps.cpp:872-924, 244-249`, `src/Ops/WindowOps.cpp:601-611, 1188-1198` |
| 2.10 | **TranslateCoordinates always returns child=None.** AWT's XDnD descends the hierarchy via this field → drop-target discovery terminates at the toplevel. Reuse the deepest-child pick already in QueryPointer. CONFIRMED | High | `src/Ops/QueryOps.cpp:823-828` |
| 2.11 | **Ctrl+click state machine (Swift)** — likely the reported regression: modern AppKit delivers Ctrl+left-click as `rightMouseDown` only, so the suppression flag is never armed → button 3 emitted. Worse traps: a latched flag can swallow the next genuine right-click; a mouseDown/rightMouseUp mismatch leaves button 1 stuck pressed → `drag_xid` never clears → **all** motion routes to a stale window. Decide button identity once per physical press; make down/up symmetric. CONFIRMED code, AppKit ordering PLAUSIBLE | High | `SwiftX11/UI/Windows/X11WindowHost.swift:1053-1097` |
| 2.12 | **GrabKey/UngrabKey no-ops**; GrabPointer accepts window 0 creating a black-hole grab that eats all pointer input; confine_to/cursor discarded; ChangeActivePointerGrab allows non-owners. CONFIRMED | Med–High | `src/Ops/GrabOps.cpp:89-97, 292-299` |
| 2.13 | **Timestamps**: each sender calls `x11_now_ms_monotonic()` independently and the CAS +1 bump means one physical event yields multiple different timestamps (and bursts run the clock ahead). Sample once per host command, pass down. Also SelectionNotify/SelectionClear sent with time=0 (ICCCM forbids). CONFIRMED | Medium | `src/Core/timestamp.cpp:49-68`, `src/Ops/SelectionOps.cpp:88, 704` |
| 2.14 | Others: KeymapNotify never generated (modifier desync after refocus); PointerMotionHint ignored; key events carry root coords in event_x/y and child=0; motion child field always 0; horizontal scroll (`axis==1`) discarded — dead horizontal trackpad scroll in waveform panes; keyboard map hardcoded US with no MappingNotify broadcast; XI2 FP16.16 uses UB left-shift on negatives (multi-monitor negative coords). CONFIRMED | Medium | `src/Ops/EventOps.cpp:499-504`, `src/XProtoServerBridge.cpp:908-909`, `src/Ops/MiscOps.cpp:76-81` |

Verified GOOD: `grabWantsMotion()` (the .62 fix) is spec-correct — it just isn't applied to the implicit-grab path (2.6); ButtonPress state-before-transition; SendEvent's 0x80 bit and seq restamp; `mach_continuous_time` clock choice.

---

## 3. The small-dialog bug — remaining confirmed mechanisms (ranked)

The deferred-map/peak/hints machinery fixed the common orderings, but six escape paths remain. Ordered by likely share of the residual reports:

1. **WM_NORMAL_HINTS handler shrinks a mapped, correctly-sized dialog** — `desiredLarger` ORs the axes, so hints with one larger axis force the window to the *hint pair* in **both** axes, at any time, even mapped, with **no ConfigureNotify** (only a UI push). AWT often commits hints after `setVisible`: Configure(800×400) → Map → hints(PMinSize 500×450) → window silently 500×450, client still painting 800×400. Intermittent by client thread timing. Fix: never resize mapped windows from hints; per-axis grow-only for unmapped tiny ones; always emit synthetic ConfigureNotify if the server resizes. CONFIRMED — `src/Ops/PropOps.cpp:267-284`.
2. **Second surface reallocation drops the paint with no re-Expose** — `surface_resize_exposed` is one-shot per window lifetime; only the first size change triggers the re-expose (case A). Client-driven ConfigureWindow sends Expose *immediately* but `setContentSize`/surface realloc happen later on main-thread async → client paints new size into the old small surface (clipped), then the realloc (case B, "live resize — skip") never re-exposes, and `applyRootlessResize` early-returns (geometry already set) so no rescue. Right-sized NSWindow, white/clipped content. Also hits re-map after withdrawal. Fix: re-expose on any surface growth outside genuine Cocoa live-resize; reset the flag on unmap. CONFIRMED — `src/XProtoServerBridge.cpp:436-474`, `src/Core/WindowTable.cpp:159-165`, `SwiftX11/Core/WindowRegistry.swift:1299-1363`.
3. **Cocoa echo after suppression-budget exhaustion walks geometry back** — after 12 non-matching `windowDidResize` callbacks, suppression gives up and the next callback echoes Cocoa's current (possibly stale rescue-sized) frame into `applyRootlessResize` → real ConfigureNotify at the stale size → Swing adopts it → dialog small and stays small. Style-mask churn in `applyWindowType` (borderless↔titled flips) generates exactly such extra callbacks. Related: `shouldSuppressRootlessResize` has **zero callers**, and a 1px scale-rounding mismatch can ping-pong. CONFIRMED — `SwiftX11/Core/WindowRegistry.swift:1177-1206, 1730`, `SwiftX11/UI/Windows/X11WindowHost.swift:1414-1418`.
4. **flushPendingMaps fires on any socket-drain gap** — it runs whenever `readAndDispatch` returns `NeedMore`, i.e. whenever the kernel buffer momentarily empties. TCP segmentation between MapWindow and the trailing ConfigureWindow/hints → rescue resolves with no data → 500×300 fallback + forced centering. Compounding: **peak tracking is disabled during the pending window** because `setMapped(true)` happens before the deferral and `notePeakSize` only records for unmapped windows — so the Configure(real)→Configure(tiny) walk-back arriving after MapWindow leaves no peak at all. Fix: flush on deadline or sync-request boundary, not first drain; track peak while `wid ∈ pending_maps_`. CONFIRMED — `src/Transport/XProtoDaemon.cpp:378-381`, `src/Core/XProtoServer.cpp:360-570`, `src/Ops/WindowOps.cpp:847`, `src/Ops/WindowAttrOps.cpp:475-477`.
5. **Stale peak + undeliverable rescue ConfigureNotify** — peak is cleared only by flush; unmap/destroy/non-deferred maps leave it. A withdrawn dialog legitimately reconfigured smaller and re-mapped trips "shrunk-below-peak" → forced back to the stale size and **recentered** (the rescue also unconditionally recenters, discarding client position). And the rescue ConfigureNotify rides the one-shot `didNotifyPresentable` SetPresentable, which never re-fires on re-map → client never told. XID reuse inherits the ghost's peak and `needs_post_map_configure_notify_`. CONFIRMED — `src/Core/XProtoServer.cpp:433-441, 505-521, 533-534`, `SwiftX11/UI/Windows/X11WindowHost.swift:107, 456-464`.
6. **Rescue ConfigureNotify isn't marked synthetic** — ICCCM requires WM-generated ConfigureNotify to set the SendEvent bit (`ev[0] = 22|0x80`) and Swing branches on it for inset/coordinate interpretation; combined with the fabricated `_NET_FRAME_EXTENTS top=28`, AWT can subtract phantom insets. Also `_NET_FRAME_EXTENTS` is stored with **type=ATOM** instead of CARDINAL, and the GetProperty type-mismatch reply is malformed (§6.2) → Java sees "no property". One-byte + one-constant fixes. CONFIRMED — `src/XProtoServerBridge.cpp:397-413`, `src/Core/XProtoServer.cpp:558-559`.

Supporting defects: Expose is sent before the queued ConfigureNotify on ConfigureWindow (client paints once at stale size) and unconditionally (unmapped windows, no ExposureMask, move-only configures); USSize flag (0x02) is ignored at both hint-resolution sites so user-specified geometry escapes; PMaxSize never clamps the rescue/fallback; hints set on a *child* window resize the host via `topLevelAncestorOf`; PPosition with obsolete-zero fields moves windows to (0,0) then the main-screen adjuster relocates them; `clampDescendantsToParent` permanently drags children to the edge after a transient shrink; `flushPendingMaps` will map a window the client already unmapped (ghost NSWindow); the 2×-area threshold both under-rescues (1.6× walk-backs stay small) and over-rescues (legitimate pre-map shrinks are reverted, hints ignored when peak is "trusted unconditionally").

---

## 4. RENDER / drawing correctness

| # | Finding | Sev | Where |
|---|---|---|---|
| 4.1 | **PutImage ZPixmap→pixmap is silently dropped** (window path requires window XID; pixmap path only accepts XY/depth-1). Pixmaps init to 0xFFFFFFFF, and the mask sampler reads alpha → Java2D's XRender AA masks become fully-opaque → **every antialiased Java2D shape fills its bounding box solid**. Cairo "similar"-surface uploads also vanish. This is the single biggest rendering lie for Vivado. CONFIRMED | Critical | `src/Ops/DrawOps.cpp:192-204, 322-325`, `src/Core/PixmapTable.cpp:50` |
| 4.2 | **SetPictureTransform / SetPictureFilter silently ignored** — all composites/gradients sample identity. Java2D uses transforms for every scaled `drawImage`; Cairo sets a matrix on every gradient. Wrong-scale/wrong-origin rendering with no error. CONFIRMED | Critical | `src/Ops/RenderOps.cpp:2086-2089, 2120-2123` |
| 4.3 | **CopyArea never sends GraphicsExpose** — unconditional NoExpose even when the copy was clamped/occluded. Swing `JViewport` BLIT scrolling relies on GraphicsExpose to repaint scrolled-in regions → stale-pixel scroll corruption in tree/wave views. CONFIRMED | High | `src/Ops/DrawOps.cpp:779-783, 989-993` |
| 4.4 | **GetImage returns silently clipped short replies** instead of BadMatch; Xlib builds the XImage at requested geometry over a short buffer → client-side garbage/overread. Planemask ignored, XYPixmap → BadValue, depth hardcoded 24. CONFIRMED | High | `src/Ops/DrawOps.cpp:426-461` |
| 4.5 | **AllocColor(black) returns a random dark-red pixel** — `pixel==0` triggers the alloc-fallback path. Pixel 0 is valid TrueColor black. Motif/GTK-L&F text drawn dark-red. Trivial fix. CONFIRMED | High | `src/Ops/ColorOps.cpp:263-264` |
| 4.6 | **Trapezoids render non-solid sources as opaque black** (no 1×1-repeat promotion, no gradient sampling, xSrc/ySrc discarded) — GTK rounded/gradient fills come out black. Composite already has the needed machinery. CONFIRMED | High | `src/Ops/RenderOps.cpp:993-1001` |
| 4.7 | **Core ops force alpha=0xFF on pixmaps too**, destroying RENDER alpha (cairo XCopyArea between ARGB32 surfaces); composite sampling is format-blind (RGB24 top byte treated as real alpha). Force opaque only for window destinations; honor picture format on sampling. CONFIRMED | Med-High | `include/Utils/RasterOp.hpp:53`, `src/Ops/DrawOps.cpp:711-743`, `src/Ops/RenderOps.cpp:944-961` |
| 4.8 | **RENDER CreateCursor/CreateAnimCursor consume the XID without creating anything** — GTK3/4 make all cursors this way → apps stuck on arrow. Also core pixmap cursors always render as Arrow, RecolorCursor no-op (Swift side). CONFIRMED | Medium | `src/Ops/RenderOps.cpp:2080-2083, 2126-2129`, `src/Core/CursorRouting.cpp:31-40` |
| 4.9 | **CompositeGlyphs treats len==0 elt as terminator** — spec uses empty elts to carry large deltas; long runs at >32k offsets truncate. `continue` not `break`. CONFIRMED | Medium | `src/Ops/RenderOps.cpp:1760` |
| 4.10 | RepeatPad/Reflect degrade to tile (edge smearing on GTK scaled patterns); component-alpha bit unparsed and the maskFormat glyph path collapses ARGB32 glyphs to max(R,G,B) grayscale (the .54 LCD path only runs when maskFormat==0); radial gradients ignore center offset; ReferenceGlyphSet deep-copies instead of sharing; AddTraps errors BadRequest (Xlib default handler exits). CONFIRMED | Medium | `src/Ops/RenderOps.cpp:594, 1855-1937` |
| 4.11 | **GC**: pixel 1 hardcoded to white (color #000001 becomes white); **clip-mask pixmaps ignored entirely** (Motif/Xt bitmap clips overdraw); GC font never validated. Depth-1 pixmaps only writable via PutImage(XY)/FillRect/FillArc/CopyPlane — lines/polys/CopyArea to bitmaps silently dropped (stipple/shape masks come out empty). CONFIRMED | Medium | `src/Ops/GCOps.cpp:43-47, 81-84`, `src/Core/DrawableRW.cpp:361-364` |
| 4.12 | SHAPE: unknown/wrong-depth mask pixmap **clears** the existing shape instead of BadMatch; ShapeCombine Invert acts as Set; clip-kind combines against bounding base; ShapeSelectInput swallowed, ShapeNotify never sent. Core SHAPE set/query paths otherwise real and correct. CONFIRMED | Medium | `src/Ops/ExtensionOps.cpp:305-308, 423-425`, `src/Core/ShapeRegion.cpp:114-118` |

Verified GOOD: premultiplied Over math in `applyOp`/`compositeOver`; QueryPictFormats wire layout and mask consistency with the advertised visual; per-elt glyphset switching and delta accumulation; FillRectangles all 13 ops; dst clip honored per-pixel; Composite self-overlap snapshot.

---

## 5. Extension advertisement honesty

| Ext | Advertised | Reality | Action |
|---|---|---|---|
| **XTEST** | **2.2** (docs claim 2.0!) | FakeInput/GrabControl no-ops; CompareCursor always "same" | **Verify the missing downgrade (§0).** Then either wire FakeInput into the host-command input path (all machinery exists) or stop advertising — Robot/xdotool currently hang silently |
| **DAMAGE** | 1.1 | No damage objects; **DamageNotify never sent** — clients that wait, wait forever | Stop advertising until real |
| **Composite** | 0.4 | All minors swallowed; **NameWindowPixmap discards the client's pixmap XID** → delayed BadPixmap three requests later; GetOverlayWindow returns the root | Stop advertising, or error honestly |
| **XFIXES** | 5.0 | No region objects (region ops swallowed, FetchRegion empty); **SelectSelectionInput swallowed, XFixesSelectionNotify never sent** (GTK clipboard watch silently dead); GetCursorImage returns 1×1 transparent | Drop to 1.0 + implement selection events (SelectionOps already tracks owners), or unadvertise |
| **RANDR** | 1.3 | Geometry real (good); but RRSelectInput untracked yet RRScreenChangeNotify broadcast to everyone; rotation byte 0 (RR_Rotate_0 is **1**); all timestamps 0; 1.0/1.1 minors (GetScreenInfo/SetScreenConfig) → BadRequest inside the advertised range; no EDID/gamma | Track selection, rotation=1, monotonic config timestamps, answer GetScreenInfo |
| **RENDER** | 0.11 | Real rasterizer, but see §4.2/4.6/4.8 | Fix or document honestly |
| **XINERAMA** | 1.1 | Real, consistent with RANDR (verified — important for Java multi-monitor) | Keep |
| **SHAPE** | 1.1 | Real except §4.12 | Keep, fix nits |
| **XC-MISC** | 1.1 | Real allocation but **no recycling** and hands out midpoint-up IDs that Xlib may already have used; contradictory comments | Serve from verified-free lists |
| **BIG-REQUESTS** | 4MB | Cap not enforced on read path (§1.6) | Clamp |
| **GE** | 1.0 | Fine while XI2 hidden | Keep |
| **MIT-SHM / XKB** | hidden | Correct calls — Xlib/Java fall back cleanly. **Do not add a partial XKB stub later** (Xlib switches its whole keycode translation to XKB); if SHM is ever added it must be per-transport (never for TCP/Docker clients) | Keep hidden; add code comment |
| **XI2** | hidden, but **opcode 141 fully answers** and XI1 GetExtensionVersion says present=1 | See below | Gate dispatch on the same hide flag |

**XI2 re-enable path** (the framing is NOT the problem — event sizes/lengths verified correct against XI2proto):
1. Make selections per-(client, window, device): replace the global `xi2_root_mask` (which overwrites on each select and delivers by window-owner) — this is the actual "Electron corruption": portal-gtk received 84-byte GenericEvents it never negotiated while Electron starved. `include/Core/InputState.hpp:44`, `src/Ops/ExtensionOps.cpp:1256-1263`, senders at `src/Ops/EventOps.cpp:687-858`.
2. Fix XIQueryPointer: buttons_len written at offset 36 (= `mods.base_mods`) instead of 34 → every reply claims Shift held. `src/Ops/ExtensionOps.cpp:1203-1223`.
3. Fix ScrollClass attached to valuators 0/1 (position axes) instead of dedicated 2/3, while motion events declare valuators_len=0 — advertised smooth scroll can never be delivered and GTK may interpret position deltas as scroll. `src/Ops/ExtensionOps.cpp:1410-1430`.
4. Fix FP16.16 negative-coordinate UB (`(int64_t)x * 65536`).
5. Deliver Raw events to root selectors with root as event window, via the cross-client sender (which restamps correctly).

Housekeeping: the eight files in `src/Extensions/*.cpp` (XKBOps, ShmOps, XInput2Ops, XRenderOps, CompositeOps, RandROps, XineramaOps, ExtDispatcher) are **unregistered 28-line dead stubs** — the real dispatch is in `src/Ops/ExtensionOps.cpp` and `RenderOps.cpp`. Delete or mark them; work done there silently does nothing.

---

## 6. Resource lifecycle, properties, selections, fonts

| # | Finding | Sev | Where |
|---|---|---|---|
| 6.1 | **rid_base wraps after 256 connections**: `(clientSlot & 0xFF) << 24` from a never-recycled counter. Slot 256 → base 0 (collides with the server's root XID conventions); slot 257 collides with connection #1 — Vivado's long-lived main JVM. Java opens many short-lived helper connections; multi-day sessions walk the counter up. On collision: silent cross-client corruption, because **only CreateWindow checks ID ownership** — CreatePixmap/CreateGC/CreateCursor/OpenFont perform no range/reuse checks at all (`clientOwnsXid` has one call site). CONFIRMED | Critical | `src/Transport/XProtoDaemon.cpp:441-445` |
| 6.2 | **GetProperty type-mismatch reply malformed** (returns format=0/bytes_after=0; spec: actual format + full length so clients can distinguish absent vs wrong-type) — and the server plants exactly such a mismatch itself: `_NET_FRAME_EXTENTS` stored with type=ATOM instead of CARDINAL → Java computes zero insets. Also: delete-on-read skipped when long_offset>0 (**stalls INCR transfers mid-stream** — large paste into Java freezes); zero-length properties reported as nonexistent (breaks the INCR terminator, which *is* a zero-length property). CONFIRMED | High | `src/Ops/PropOps.cpp:522, 533-540, 607`, `src/Core/XProtoServer.cpp:558-559` |
| 6.3 | **Dead selection owners never cleared** (DestroyWindow/removeClient don't touch `sSelOwner`): ConvertSelection to a corpse drops the SelectionNotify → requestor blocks for its full timeout on every paste; recycled XIDs become accidental owners. CONFIRMED | High | `src/Ops/SelectionOps.cpp:39-55` + removeClient |
| 6.4 | **Client death leaks everything but windows**: pixmaps (w×h×4 each; Java double-buffers heavily), GCs, cursors, fonts — `FontTable::eraseOwnedBy` exists with **zero callers**. Unbounded growth over connect/disconnect cycles. CONFIRMED | High | `src/Transport/XProtoDaemon.cpp:488-606` |
| 6.5 | **Grab-release holes**: `removeForWindows` never checks the keyboard grab (a dead client's keyboard grab persists forever); the RetainPermanent path skips grab cleanup entirely — an XDND helper dying mid-drag (its signature move) leaves the active pointer grab installed → all pointer input frozen. CONFIRMED | High | `src/Core/GrabTable.cpp:110-124`, `src/Transport/XProtoDaemon.cpp:549-573` |
| 6.6 | **KillClient is a stub** and RetainTemporary ≡ RetainPermanent → retained/orphaned windows are immortal (every XDND helper deposits permanent proxies). Also retained windows (owner_fd=-1) can't receive events and — due to a same-owner parent check that itself violates the spec — **can't have children created under them**, undermining the exact XDND scenario RetainPermanent was built for. CONFIRMED | High | `src/Ops/MiscOps.cpp:270-275`, `src/Ops/WindowOps.cpp:473-476` |
| 6.7 | **Stale state on destroyed/recycled XIDs**: PropertyTable entries survive destroy (ghost WM_PROTOCOLS → recycled windows get WM_DELETE ClientMessages; permanent leak), peak sizes (§3.5), `needs_post_map_configure_notify_`, Swift resize-suppression state; **DestroyWindow doesn't recurse** — children keep dangling parents and leak. CONFIRMED | High | `src/Ops/WindowOps.cpp:581-651`, `src/Core/WindowTable.cpp` erase |
| 6.8 | **PropertyNotify only reaches the window owner** (single event_mask per window; a second client's ChangeWindowAttributes clobbers the owner's mask — can strip the JVM's PropertyChangeMask and silently kill Java's timestamp extraction → copy breaks). Spec: deliver to every selecting client. Same single-mask model breaks SendEvent's mask semantics. ARCHITECTURAL — same fix as XI2 §5.1. CONFIRMED | High | `src/Ops/PropOps.cpp:38-53`, `src/Core/WindowTable.cpp:510-525` |
| 6.9 | **OpenFont can never fail** (any name falls through to a default; no BadName) and QueryFont on an invalid fid falls back to "fixed" — Java's open-and-check font probing concludes every font exists with fallback metrics. Also fabricated per-char metrics (`allCharsExist=1`, invented 8×11 cells where QueryFont ≠ drawn advance → column drift) and an undocumented 2× Retina scaling of XLFD pixel sizes (internally consistent but surprising). CONFIRMED | Med-High | `src/Ops/FontOps.cpp:132-145, 173-174, 195-215`, `src/Core/FontTable.cpp:120-127, 424-579` |
| 6.10 | Selections, misc: same-connection ConvertSelection blanket-refused (breaks PRIMARY self-paste in single-client apps); forced CLIPBOARD takeover downgrades all inter-client transfers to UTF-8 text (two-JVM setups lose rich flavors); MULTIPLE unsupported yet INCR wrongly listed in TARGETS; SetSelectionOwner stores CurrentTime verbatim (TIMESTAMP target can return 0 — ICCCM forbids); ChangeProperty append with mismatched type silently replaces (spec: BadMatch); RotateProperties still a no-op despite v1.2.0 notes; property cap is actually 48MB (`3<<24`) not the documented 16MB and silently truncates instead of BadAlloc. CONFIRMED | Medium | `src/Ops/SelectionOps.cpp:285-292, 424-433, 510-530, 691-709`, `src/Core/PropertyTable.hpp:56-63, 110` |
| 6.11 | Predefined atoms 1–68 fully audited against Xatom.h: **all correct** (the 40/41 fix is in and its neighbors are right). GC-fabrication on unknown XIDs (`getOrCreate`) masks BadGC; FreeGC/FreeCursor silently lenient; CreateCursor stores pixmap XIDs without copying — the universal create-cursor-then-free-pixmap pattern leaves dangling references (currently masked only because pixmap cursors all render as Arrow). CONFIRMED | Low-Med | `src/Core/AtomTable.cpp:22-91`, `src/Core/GCTable.cpp:17-27`, `src/Core/CursorTable.cpp:12-29` |

---

## 7. Symptom → root-cause map

| Symptom you've been chasing | Most likely causes (in order) |
|---|---|
| Dialogs turn up small (intermittent) | §3.1 hints-shrink-mapped; §3.2 lost paint on 2nd surface realloc; §3.3 Cocoa echo walk-back; §3.4 drain-race + disabled peak; §3.5 stale peak on re-map |
| Vivado crash after laptop sleep | §1.2 poisoned-floor misread as SEQ_WRAP (armed by §1.3 seq=0 events or §1.5), triggered by the post-wake host-command event burst. Diagnostic: log `max_wire_seq_` vs `last_request_seq_` at SEQ_WRAP |
| XI2 "wire corruption" / Electron crash | §5 XI2: global root mask + owner-routing misdelivery (framing verified fine) + §1.4 phantom [SEQ_REGRESS] evidence |
| XTEST GrabControl crash | §0: the v2.0 downgrade isn't in the code — trigger live. Underlying: FakeInput silent no-op (client-side robot loops hang/assert) — PLAUSIBLE, untestable until version verified |
| Ctrl+click → button 3 regression | §2.11 AppKit delivers Ctrl+click as rightMouseDown only → suppression flag never armed; plus two stuck-state traps in the same machine |
| hw_ila / drag-and-drop fragility | §2.1 QueryPointer garbage mask; §2.6 implicit-grab wrong window; §2.5 grab stomping; §2.10 TranslateCoordinates child=None; §6.5 grab leak on helper death; §6.6 XDND proxy children blocked |
| Menus/tooltips/hover glitches | §2.2 keyboard grabs inert; §2.3 crossing flags; §2.7 no detail/Virtual chain; §2.9 focus revert to None |
| Scroll corruption in tree/wave views | §4.3 missing GraphicsExpose; §1.8 motion drop under backpressure |
| Java2D rendering oddities | §4.1 PutImage→pixmap dropped (AA shapes as solid boxes); §4.2 transforms ignored (scaled images); §4.5 AllocColor black; §6.2 _NET_FRAME_EXTENTS type |
| Long-session degradation | §6.1 rid_base wrap; §6.4 leaks; §6.3 selection corpses; §6.7 stale XID state; XC-MISC non-recycling |

---

## 8. Suggested remediation order

**Phase 0 — verify & hotfix (hours):**
1. Confirm XTEST GetVersion reply (docs say 2.0, code says 2.2) and re-apply the downgrade if intended.
2. One-liners: QueryPointer mask (§2.1); crossing-event byte 31 (§2.3); AllocColor pixel 0 (§4.5); `_NET_FRAME_EXTENTS` type=CARDINAL + SendEvent bit on rescue ConfigureNotify (§3.6); WindowClose event seq stamping (§1.3); CompositeGlyphs len==0 `continue` (§4.9); clamp BIG-REQUESTS length (§1.6).

**Phase 1 — small-dialog closure:** §3.1 (guard PropOps resize with `!isMapped`, per-axis grow-only), §3.2 (re-expose on surface growth outside live resize), §3.4 (peak tracking while pending; flush on deadline), §3.5 (clear peak/pending state on unmap/destroy), §3.3 (fix the echo path; wire up or delete `shouldSuppressRootlessResize`).

**Phase 2 — input correctness for Vivado:** keyboard grab routing (§2.2); grab ownership/time/status (§2.5, subsumes the planned `owner_fd` task); implicit-grab target + mask filtering (§2.6); focus revert-to (§2.9); TranslateCoordinates child (§2.10); Ctrl+click state machine (§2.11); do_not_propagate (§2.8).

**Phase 3 — wire integrity endgame:** extension reply safety net (§1.7) → structured single-writer replies (§1.5) → fix instrumentation (§1.4) → then **delete the floor and SEQ_WRAP heuristics** (§1.1/1.2). Don't delete first: the floor is currently masking §1.3-class bugs.

**Phase 4 — stop lying:** unadvertise DAMAGE/Composite (or implement); XFIXES → 1.0 + selection events; XTEST FakeInput → host-command path or unadvertise; RANDR fixes; then the XI2 re-enable path (§5) on top of per-(client,window) event masks — the same mechanism PropertyNotify (§6.8) needs.

**Phase 5 — long-session hygiene:** rid_base slot recycling (§6.1) + ID-range enforcement on all Create* ops; resource free on disconnect (§6.4); selection-owner sweep (§6.3); grab cleanup on death incl. retain path (§6.5); recursive DestroyWindow + property/peak purge (§6.7); KillClient (§6.6).

---

## 9. Verified-good list (don't "fix" these)

- Predefined atom table 1–68 — fully correct.
- XI2 GenericEvent sizes/lengths and XIQueryDevice class blocks — correct; do not rewrite the framing when re-enabling.
- Cross-client event restamping with the target's `lastSeq()` — spec-correct, keep.
- RENDER premultiplied Over math, QueryPictFormats layout, glyph elt handling (except len==0), FillRectangles op table.
- `grabWantsMotion()` three-family mask test.
- WM_NORMAL_HINTS field offsets and the 40/41 atom fix.
- Xinerama/RANDR geometry consistency (single ScreenLayout source).
- `mach_continuous_time`-based timestamps (clock family survives sleep).
- 'B' byte-order rejection; ByteReader bounds checking; core request-length caps.
