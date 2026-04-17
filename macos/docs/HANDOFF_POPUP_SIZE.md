# HANDOFF: Vivado Popup Size Race Investigation

## The Bug

Two related symptoms in Vivado popups (e.g., synthesis/implementation complete dialogs):

1. **Popups open at minimum size** — not at the real size Vivado requested.
   They come up at roughly the WM floor (200×100) or the fallback (400×300)
   instead of the actual dialog size.

2. **Snap to 1×1 on user resize** — when trying to enlarge the popup by
   dragging its edge, the window occasionally collapses to a tiny sliver
   showing only a fraction of the title bar's red close button dot.

Both symptoms suggest a race between client ConfigureWindow, WM_NORMAL_HINTS
arrival, and macOS/Cocoa window state.

## Context From Last Session

Recent work related to window sizing:
- **Deferred map race fix** (v1.19.35.37): `handleMapWindow` no longer
  sends a stale tiny Expose before `flushPendingMaps` resizes.
- **Fallback floor** (v1.19.35.37): Windows under 10px at map time with no
  WM_NORMAL_HINTS and no peak size get bumped to 400×300 instead of 1×1.
- **Metal backgrounded crash fix** (v1.19.35, released): Skip draw when
  window hidden/occluded.

The fallback-to-400×300 explains the "minimum size" symptom — if
WM_NORMAL_HINTS and peak-size both fail, we're hardcoding a small size.
That's better than 1×1 but wrong for actual dialog content.

## Investigation Plan

### Step 1: Understand what's in WM_NORMAL_HINTS at flush time

In `flushPendingMaps()` (XProtoServer.cpp ~line 375), log the raw bytes of
WM_NORMAL_HINTS when a popup is being flushed. Currently we only log the
resolved size. We need to see:
- Is the property set at all?
- Which flags are set (PSize=0x08, PMinSize=0x10, PMaxSize=0x20, PResizeInc=0x40, PAspect=0x80, PBaseSize=0x100)?
- What are the raw width/height values?

Also log the peak-size state — is it tracked and what value is stored?

### Step 2: Instrument every geometry transition

Add a `[GEOM]` trace (always-on in debug) that logs EVERY change to a host
window's WindowTable geometry, with the source:

- `[GEOM] wid=0x... source=CREATE old=0x0 new=WxH`
- `[GEOM] wid=0x... source=CONFIG vmask=0x.. old=WxH new=W'xH'`
- `[GEOM] wid=0x... source=FLUSH_MAP old=WxH new=W'xH'`
- `[GEOM] wid=0x... source=COCOA_RESIZE old=WxH new=W'xH'` (from applyRootlessResize)
- `[GEOM] wid=0x... source=WM_HINTS min=WxH max=WxH`

Files to touch:
- `WindowOps.cpp` — CreateWindow handler
- `WindowAttrOps.cpp` — ConfigureWindow handler (currently has `[CONFIGURE_TOPLEVEL]`)
- `XProtoServer.cpp` — flushPendingMaps
- `WindowOps.cpp` — applyRootlessResize (Cocoa resize → C++ update)
- `PropOps.cpp` — WM_NORMAL_HINTS application

### Step 3: Instrument the NSWindow side

When WM_NORMAL_HINTS arrives, we update NSWindow.contentMinSize/contentMaxSize
to let AppKit enforce bounds. Log every change:

- `[GEOM_NS] wid=0x... contentSize=WxH minSize=WxH maxSize=WxH`

Look for the case where user drags to resize and Cocoa shrinks the window
below what the X11 side expects — that's the "snap to 1×1" symptom.

### Step 4: Capture traces

Have the user:
1. Run Vivado with the instrumented build
2. Trigger a popup that exhibits the problem (synthesis complete dialog)
3. Try to resize it until it snaps to 1×1
4. Paste the full `[GEOM]` trace timeline for that window's XID

### Likely Root Cause Hypotheses

1. **WM_NORMAL_HINTS arrives AFTER MapWindow** — Vivado might send hints
   after mapping, so flushPendingMaps sees no hints and falls back to
   400×300. NSWindow.contentMinSize is then never set, allowing user
   resize to shrink to 0.

2. **Peak-size tracking cleared too early** — If a ConfigureWindow with
   CWWidth|CWHeight arrives while mapped, it bypasses peak tracking but
   may be transient (e.g., Java AWT's pack() cycle).

3. **Cocoa windowDidResize firing with stale size** — During rapid
   resize events, `handleDrawableSize` might get called with the
   old size from a pending layout tick, triggering applyRootlessResize
   to shrink.

4. **No contentMinSize enforcement on popups** — Windows with
   `_MOTIF_WM_HINTS decor=0` or `_NET_WM_WINDOW_TYPE=DIALOG` might
   bypass the contentMinSize path.

## Files Likely To Touch

- `X11LowLevel/cpp/X11Protocol/src/Ops/WindowAttrOps.cpp` — ConfigureWindow
- `X11LowLevel/cpp/X11Protocol/src/Ops/WindowOps.cpp` — CreateWindow, applyRootlessResize
- `X11LowLevel/cpp/X11Protocol/src/Core/XProtoServer.cpp` — flushPendingMaps
- `X11LowLevel/cpp/X11Protocol/src/Ops/PropOps.cpp` — WM_NORMAL_HINTS handler
- `SwiftX11/UI/Windows/X11WindowHost.swift` — handleDrawableSize, NSWindow config
- `SwiftX11/UI/Windows/X11WindowController.swift` — contentMinSize/Max updates

## Current Debug Build

At session end: **v1.19.35.38-dbg** (includes EOF/Error log cleanup + deferred
map race fix). The EOF fix is a clean disconnect no longer spams ring-buffer
dumps in the log — keep that for diagnostics clarity.

## Testing

Wait for a Vivado flow that reliably triggers the popup:
- Run synthesis or implementation → completion dialog should appear
- Try to resize it and watch for the collapse

The "occasionally snaps to 1×1" is the key repro — if we can trigger it
deterministically once, the `[GEOM]` trace will show which event caused the
geometry to go to tiny.
