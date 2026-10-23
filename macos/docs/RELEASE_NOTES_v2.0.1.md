# SwiftX11 v2.0.1 — release notes

Published to GitHub Releases on 2026-10-23:
<https://github.com/LawrenceGibbons/SwiftX11/releases/tag/v2.0.1>. This file is
the user-facing highlights; the exhaustive per-build changelog lives in
`docs/CLAUDE.md`.

A patch release on the v2.0.0 line with two input fixes found running Vivado 2025.1 under SwiftX11. No wire-format or extension changes.

## Fixes

- **Popup menus and menubar tracking in Java/Swing applications.** Some Vivado context menus and combo-box dropdowns opened but ignored item clicks, and hovering across the main menubar did not switch menus. The server returned `AlreadyGrabbed` when a client replaced its own XInput2 implicit grab with a core `GrabPointer`/`GrabKeyboard`; per the xorg implementation a client may always replace its own grab regardless of level, and `AlreadyGrabbed` is reserved for a grab held by a *different* client. Both the request handlers and the grab table now follow that rule.
- **Text selection anchor lost after a trackpad scroll.** Click, scroll with the trackpad, Shift+click: the selection began part-way between the two points. Finger drift on a vertical swipe leaked horizontal scroll ticks (X11 buttons 6/7), which Java's AWT treats as extra mouse-button presses rather than wheel events, re-positioning the caret mid-scroll. Trackpad gestures are now locked to their dominant axis for the duration of the gesture (including momentum), as native macOS scroll views do. Mouse wheels are unaffected, so Shift+wheel still scrolls horizontally, and a deliberate sideways swipe still produces horizontal ticks.

Synchronous grab freeze/replay remains unimplemented — asynchronous grabs, which modern toolkits use, work fully. See the bundled `KNOWN_ISSUES.md` for the full list of current limitations.

## Install

Download **SwiftX11-2.0.1.dmg**, open it, and drag **SwiftX11** to Applications. X11 bitmap fonts are bundled inside the app — no XQuartz needed for the server itself. The app is unsigned — right-click → **Open** to bypass Gatekeeper on first launch. Requires macOS 14+.
