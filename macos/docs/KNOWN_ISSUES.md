# SwiftX11 — Known Issues & Limitations

Current limitations you might encounter. Vivado, Vitis, and common X11 clients
(xterm, xeyes, xcalc, xclock) work; the items below are edge cases or features a
few clients rely on. The full developer backlog is in `docs/DEFERRED.md`.

## Input

- **Synchronous pointer/keyboard grabs are not frozen.** A client that grabs with
  `GrabModeSync` and drives the device via `AllowEvents` / `ReplayPointer` (the
  classic Motif/mwm "click-through" window-manager pattern) will not see the grab
  freeze — `AllowEvents` is accepted but does nothing. Normal apps are unaffected:
  Java/Swing (Vivado) and Electron/GTK (Vitis) use **asynchronous** grabs, which
  work fully. SwiftX11 is itself the (rootless) window manager, so the external-WM
  click-through scenario does not arise here.

- **`xinput query-state` / `xinput get-button-map` don't work.** These use the
  legacy XInput 1.x `OpenDevice` requests, which return `BadRequest` by design.
  XInput2 (the modern API used by GTK/Chromium) and ordinary keyboard/mouse input
  work normally.

## Rendering

- **No 32-bit ARGB (RGBA) visual.** SwiftX11 advertises a single 24-bit visual, so
  GTK "RGBA" / per-pixel window transparency isn't available; such apps fall back
  to opaque windows. (Non-rectangular windows via the SHAPE extension — e.g.
  xeyes — do work.)

## Clients

- **xterm `Ctrl+V` paste.** Not a SwiftX11 bug — xterm's `insert-selection`
  binding only fires when X11 focus is on its VT100 widget, and xterm doesn't
  move focus there. Use middle-click paste (Option+click on a Magic Mouse),
  `Shift+Insert`, or add to `~/.Xresources`:
  ```
  XTerm*translations: #override Ctrl<Key>v: insert-selection(CLIPBOARD,PRIMARY)
  ```

## Setup notes

- **Display `:1`.** SwiftX11 runs on display `:1` (TCP port 6001) to avoid
  conflicting with XQuartz on `:0`. Set `export DISPLAY=127.0.0.1:1` (native) or
  `DISPLAY=host.docker.internal:1` (Docker containers).
- **Gatekeeper.** The app is ad-hoc signed, not notarized — right-click → Open on
  first launch.

## Reporting

If a client misbehaves, the **Wire Trace** and **Draw Trace** toggles in the
SwiftX11 log window capture the X11 request/response and drawing streams, which
are the most useful thing to include in a report.
