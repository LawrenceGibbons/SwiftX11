# SwiftX11 v2.0.0 — release notes

Published to GitHub Releases on 2026-09-16:
<https://github.com/LawrenceGibbons/SwiftX11/releases/tag/v2.0.0>. This file is
the user-facing highlights (what was published); the exhaustive per-build
changelog — with the internal review-batch labels (R0–R6, E1/E2, C3, …) — lives
in `docs/CLAUDE.md`.

The major architectural release: **the X11 root window is now a real window on the wire** — its advertised ID changes from `1` to `0x2`, and that wire change is what makes this a major version bump. This release is the result of a broad protocol-conformance pass, verified against the xorg-server source and exercised with real X11 applications.

## Highlights

- **The root window is a real window.** Root now has its own XID (`0x2`), geometry, and per-client event masks. It reports SubstructureNotify for every top-level window (so root-level tools like `xev -root` and `wmctrl` work), the input-delivery path reaches root-level event selections, and the server interprets and acts on the EWMH/ICCCM client messages sent to root — `_NET_ACTIVE_WINDOW`, `_NET_WM_STATE` (maximize / fullscreen / iconify), `WM_CHANGE_STATE` — while advertising `_NET_SUPPORTED` / `_NET_SUPPORTING_WM_CHECK`. `DestroyWindow(root)` and `UnmapWindow(root)` are silent no-ops, as on a real server.
- **RENDER extension completeness.** Picture transforms and filters (nearest and bilinear sampling), Pad/Reflect repeat modes, depth-8 (A8) mask upload, a proper RENDER error base, NoExpose for pixmap-to-pixmap copies, and alpha-preserving CopyArea into 32-bit pixmaps.
- **Protocol hygiene.** BIG-REQUESTS now advertises the full ~16 MB limit (a large PutImage no longer breaks the connection), XFIXES reports the version it actually implements (1.0), RANDR gains the 1.0 screen-configuration requests, and an unknown opcode correctly answers BadRequest.
- **Dynamic keyboard mapping (XKEYBOARD).** The XKB keymap now rebuilds when a client changes the keyboard mapping (e.g. via `xmodmap`) and emits XkbMapNotify, so XKB-aware clients stay in sync with the core keymap instead of keeping the boot-time layout.
- **Window management.** ReparentWindow emits the correct UnmapNotify/MapNotify sequence; InputOnly windows are fully supported (validated, and no longer obscure the window beneath them); a client may create a window under another client's window (XEmbed-style embedding); and ChangeSaveSet plus embedder-death rescue keep an embedded window alive when the client that embedded it exits.
- **Robustness.** A single malformed request can no longer tear down the server; GetImage validates against the window's own geometry (fixing an image-capture regression on partially off-screen windows); resource cleanup on window destruction and on client disconnect is more complete; and a client waiting on graphics-exposure events no longer hangs on a degenerate copy.

Synchronous grab freeze/replay is not implemented — asynchronous grabs, which modern toolkits use, work fully. See the bundled `KNOWN_ISSUES.md` for the full list of current limitations.

## Install

Download **SwiftX11-2.0.0.dmg**, open it, and drag **SwiftX11** to Applications. X11 bitmap fonts are bundled inside the app — no XQuartz needed for the server itself. The app is unsigned — right-click → **Open** to bypass Gatekeeper on first launch. Requires macOS 14+.
