# SwiftX11 v2.0.0 — release notes (running draft)

> **Draft.** Accumulated from the v2.0.0.x-dbg line and updated after each
> *verified* step. Copy into the GitHub release when v2.0.0 ships (flip
> `SWIFTX11_DEBUG_BUILD` to 0). The exhaustive per-build changelog lives in
> `docs/CLAUDE.md`; this file is the user-facing highlights.

The major architectural release: **the X11 root window is now a real window on
the wire.** Its XID changes from `1` to `0x2` — that wire change is what makes
this a major version bump. Built on v1.21.0; every item below is verified
against the xorg-server source with automated protocol probes, plus Vivado and
Vitis confirmed working.

## Highlights

### Root window as a real window (review Cluster A / R1) — verified 2026-09-10
The root window is now a first-class window with its own XID (`0x2`), geometry,
and per-client event masks instead of a bare sentinel value — so SwiftX11
behaves like a real X server for everything that targets root:
- **SubstructureNotify on root** — `xev -root`, `wmctrl`, and Java's XToolkit
  now see Create / Map / Configure / Unmap / Destroy for every top-level window,
  including xorg-style Unmap + Destroy teardown when a client disconnects.
- **Events reach root** — the button / key / motion / crossing / focus delivery
  walks climb through to root, so root-level selections (pagers, monitors) work.
- **EWMH window management** — the server interprets `_NET_ACTIVE_WINDOW`,
  `_NET_WM_STATE` (maximize / fullscreen / modal / hidden) and `WM_CHANGE_STATE`
  (iconify) ClientMessages sent to root, and advertises `_NET_SUPPORTED` /
  `_NET_SUPPORTING_WM_CHECK` so clients recognise a compliant window manager.
- **Root is protected** — `DestroyWindow(root)` and `UnmapWindow(root)` are
  silent no-ops (as on a real X server), so a malformed request can't tear the
  server down (R6.1, verified 2026-09-16).

### RENDER extension completeness (review R3) — verified 2026-09-11
- Picture **transforms + filters** (SetPictureTransform / SetPictureFilter, with
  nearest and bilinear sampling), **RepeatPad / RepeatReflect** sampling modes,
  **depth-8 A8 mask** upload via PutImage, a proper **RENDER error base**
  (BadPicture / BadPictFormat / …), NoExpose delivery for pixmap CopyArea, and
  alpha-preserving CopyArea into depth-32 pixmaps.

### Protocol hygiene (review R4) — verified 2026-09-14
- Removed the dead wire-sequence-floor rewrite machinery; tightened
  **BadLength / BadRequest discipline** — a legitimately large PutImage no
  longer kills the JVM connection (the BIG-REQUESTS limit now matches xorg's
  ~16 MB), and an unknown opcode answers BadRequest.
- **XFIXES reported as 1.0** (the version actually implemented), **RANDR 1.0
  GetScreenInfo / SetScreenConfig** implemented (JDK display-mode path),
  monotonic RANDR reply timestamps, and 7 dead extension-stub files removed.

### Keyboard — XKB dynamic map (review R5) — verified 2026-09-16
- **The XKB keymap rebuilds on a mapping change.** An `xmodmap`
  (ChangeKeyboardMapping / SetModifierMapping) now re-derives the XKB model and
  emits `XkbMapNotify`, so XKB-path clients (GTK3) no longer permanently desync
  from core-path clients.

### Window management (review R5) — verified 2026-09-16
- **ReparentWindow now matches xorg.** Reparenting a mapped window emits the
  UnmapNotify / MapNotify pair around the move (needed by XEmbed handshakes and
  subtree observers, previously silent), and a bogus new parent is rejected with
  BadWindow instead of corrupting the window tree.
- **InputOnly windows are real.** The window class is stored, validated, and
  reported (GetWindowAttributes was hardcoded to InputOutput); an InputOnly
  overlay no longer blanks the window beneath it (GTK3/GDK create these
  pervasively). Creating a window under another client's window is now allowed
  (the XEmbed / portal embedding pattern).
- **SaveSet.** ChangeSaveSet is implemented, and when a client that embedded
  another client's window dies, that window is rescued (reparented to root)
  instead of being orphaned — the embedder-crash robustness a real X server
  provides.

## Status
The entire 2026-09-08 protocol review (R0–R5) is complete and verified —
root-as-a-real-window, RENDER completeness, protocol hygiene, and the
window-management / keyboard structural work. Sync-grab freeze / replay (C3) is
deliberately deferred (near-zero value for this project's async-grab clients;
documented in CLAUDE.md). Vivado and Vitis confirmed working throughout.

A 2026-09-16 pre-release review then flagged a short "R6" list of sharp edges to
clear before tagging. **R6.1** — the one critical item, a server-killing
`DestroyWindow(root)` — is **fixed and verified** (see the root-window section).
The remaining R6 items (a GetImage occlusion regression, three long-session
resource-leak / hang protections, and minor hygiene) are tracked in
`docs/CLAUDE.md`; clearing them or accepting them as documented known issues is
the last step before the v2.0.0 tag.

## Install
_(filled in at release: `SwiftX11-2.0.0.pkg`, unsigned — right-click → Open;
requires macOS 14+.)_
