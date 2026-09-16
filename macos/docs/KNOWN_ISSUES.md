# SwiftX11 — Known Issues & Limitations

Current limitations you might encounter. Vivado, Vitis, and common X11 clients
(xterm, xeyes, xcalc, xclock) work; the items below are edge cases or features a
few clients rely on. The full developer backlog is in `docs/DEFERRED.md`.

## Stability

- **Possible crash after the Mac wakes from sleep.** SwiftX11 has historically
  been seen to crash some time after the Mac sleeps and wakes. A suspected
  contributing factor (a wire-sequence rewrite) was removed in v2.0.0.19, but
  that change shipped for a different reason and there is **no confirmed
  post-sleep fix yet** — treat this as open until it has survived real
  overnight sleep/wake cycles. If the server disappears after the Mac has
  slept, relaunch it (connected clients will need to reconnect).

- **Only one instance at a time.** Launching a second SwiftX11 while one is
  already running collides on display `:1` (both bind TCP port 6001); there is
  no single-instance lock or "reopen the running one" handling. Run a single
  instance.

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

- **Some portal-GTK dialog buttons render with slightly off colours.** In Vitis's
  GTK portal file dialogs a few buttons can look faintly inverted/tinted. Under
  investigation; the dialogs are fully functional.

## Clients

- **Vivado JIDE tab labels can show stale/duplicated text.** In Vivado's JIDE
  tabbed panels (e.g. the Hardware Manager VIO panel) a *selected* tab may render
  a doubled label such as `placeholderhw_vio_4`, correct when unselected. This is
  **strongly believed to be a JIDE/Swing upstream bug, not SwiftX11** — the server
  copies the client's blitted image verbatim, so it would reproduce on any X
  server. No SwiftX11 fix is expected.

- **Column-reorder drag in some Swing tables may leave stale pixels.** Dragging to
  reorder table columns in a few Java/Swing panels (seen in the License Manager)
  has an unverified redraw artifact flagged for retest; if you see leftover column
  pixels after a reorder, any repaint (resize or scroll) clears them.

- **MULTIPLE selection target not implemented.** Multi-target clipboard
  conversions (e.g. `xsel -m`) fail; ordinary copy/paste works.

- **xterm `Ctrl+V` paste.** Not a SwiftX11 bug — xterm's `insert-selection`
  binding only fires when X11 focus is on its VT100 widget, and xterm doesn't
  move focus there. Use middle-click paste (Option+click on a Magic Mouse),
  `Shift+Insert`, or add to `~/.Xresources`:
  ```
  XTerm*translations: #override Ctrl<Key>v: insert-selection(CLIPBOARD,PRIMARY)
  ```

## Window management

- **Cross-top-level reparenting / embedder-death rescue is X11-only.** A window
  moved by `ReparentWindow` from one top-level to another, or an embedded window
  rescued when the client that embedded it dies, is updated in the X11 window
  tree but **not** in the macOS window layer: such a window can end up
  permanently invisible or leave a stale native window behind. This is rare
  (XEmbed / portal embedding); the common cases — an `InputOnly` overlay or a
  child embedded under a live parent — work.

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
