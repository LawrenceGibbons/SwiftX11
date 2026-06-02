# SwiftX11 — Handoff (v1.19.36)

## Context

SwiftX11 is a native macOS X11 protocol server (Swift + C++). It implements
the X11 wire protocol, 101 core opcodes, and 12 extensions. It successfully
runs xterm, xeyes, xcalc, xclock, xfd, Xilinx Vivado 2024.1 / 2025.1 (Java
Swing, including the License Manager and hw_ila waveform debug), and Xilinx
Vitis 2025.1 (Electron / Theia) from an AlmaLinux 9 Docker container.

The release tag for this handoff is **`v1.19.36`** on `main`. Per-feature
history lives in `docs/CLAUDE.md`; comprehensive roadmap and open issues
live in `docs/TODO.md`.

## Status snapshot

| Client | State |
|---|---|
| **xterm** with scrollbar | ✅ Working (cursor blink, scroll, Option+click for middle-button paste, Shift+Insert) |
| **xeyes** | ✅ Working (SHAPE transparency, RawMotion via XI2) |
| **xcalc**, **xclock**, **xfd** | ✅ Working |
| **Vivado main GUI** | ✅ Working (multi-monitor, dialogs, JidePopup, IP gen, clipboard) |
| **Vivado License Manager 2024.1** | ✅ Working as of `v1.19.36` (ARGB32 component-alpha glyphs) |
| **Vivado hw_ila drag-and-drop** | ✅ Working as of `v1.19.36` (`ButtonMotionMask` routing + OR exemption + RetainPermanent) |
| **Vitis 2025.1 (Electron/Theia)** | ✅ Working (menus, dialogs, portal-GTK file dialogs — XI2 hidden by default) |

## Development conventions

### Building & Testing
- **Build**: User builds from Xcode (Cmd+B on `SwiftX11` target).  Do NOT
  use `xcodebuild` from CLI — it interferes with Xcode.app.
- **Git workflow**: Claude Code works in a worktree under
  `.claude/worktrees/<branch>/`.  Edit files there, commit, then merge to
  `develop`: `cd /Users/lkg/Documents/Vivado/SwiftX11/macos && git merge
  <branch> --no-edit` (or `--no-ff --no-edit` to keep the merge commit).
- **CRITICAL**: Do NOT edit files directly in the main repo while Xcode
  has the project open — Xcode will crash if source files change under
  it.  Always edit in the worktree.
- **Always merge to `develop`** before asking the user to test.  Leave
  `main` alone until release.  Releases are merge commits from `develop`
  to `main` followed by an annotated `vX.Y.Z` tag.
- **Version bumps**: Edit `X11LowLevel/include/SwiftX11Version.h`.  For
  debug iterations, increment `SWIFTX11_DEBUG_BUILD` (banner reads
  `vX.Y.Z.N-dbg`).  For releases, bump `SWIFTX11_VERSION_BASE` and set
  `SWIFTX11_DEBUG_BUILD = 0`.

### Trace tiers (post v1.19.36 cleanup)
1. **Always-on**: version banner, `[X11]` listen/disconnect, `[X11_ERROR]`,
   `[SCREEN]`, `[DISPLAY]`, `[QueryExtension]`, font registration summary.
2. **`#ifndef NDEBUG`** (Debug builds only — Xcode "Debug" scheme defines
   it; Release does not): `[GEOM]`, `[CONFIGURE_TOPLEVEL]`, `[PEAK_SIZE]`,
   `[KEY_SEND]`, `[BTN_SEND]`, `[CURSOR]`, `[CREATE_OR]`, `[SEL]`,
   `[CLIPBOARD]`, `[LIFECYCLE]`, `[BG_FILL]`, `[BG_FILL_RETRY]`,
   `[BORDER]`, `[CREATE_TOPLEVEL]`, `[GEOM_NS]` (Swift side via `#if DEBUG`).
   Uses `TS_DBG(...)` macro (no-op when NDEBUG defined) in
   `Utils/MachTime.hpp`, or explicit `#ifndef NDEBUG TS_FPRINTF(...) #endif`.
3. **Opt-in via `-DX11_TRACE_<CAT>` in `OTHER_CPLUSPLUSFLAGS`**:
   `X11_TRACE_RESIZE`, `X11_TRACE_PRESENT`, `X11_TRACE_LIFECYCLE`,
   `X11_TRACE_INPUT`, `X11_TRACE_RESOLVE`, `X11_TRACE_FONT`,
   `X11_TRACE_RENDER`, `X11_TRACE_WIRE`, `X11_TRACE_DRAG`,
   `X11_TRACE_VERBOSE` (= all).
4. **UI toggle**: Settings → Debug → "Wire Trace (stderr)" enables
   per-packet wire trace at runtime (atomic flag `g_wire_trace`).

### Regression tests
- `DISPLAY=127.0.0.1:1 xterm -sb -rightbar -bc` — text, scrollbar, borders
- `DISPLAY=127.0.0.1:1 xeyes` — shape extension, motion tracking
- `DISPLAY=127.0.0.1:1 xcalc` — button widgets, focus, cursor
- Vivado: `/Users/lkg/Documents/Vivado/vivado2023/launch_vivado_sx11.sh`
  — Java Swing, dialogs, JidePopup, IP generation, hw_ila drag-and-drop,
  License Manager text rendering.
- Vitis: `/Users/lkg/Documents/Vivado/vivado2023/launch_vitis_sx11.sh`
  — Electron/Theia IDE, menus, file dialogs.

### Docker container setup
- **Image**: `x64-linux-dbus` (AlmaLinux 9 + dbus + libxkbfile)
- **DISPLAY**: `host.docker.internal:1` (SwiftX11 on TCP port 6001)
- **dbus**: `dbus-daemon --session --fork --print-address` (replaced
  `dbus-launch` to avoid stale X11 root-window property)
- **Vitis `--disable-gpu`**: Injected via entrypoint patching
- **Image rebuild after entrypoint changes**: `cd
  /Users/lkg/Documents/Vivado/vivado2023 && docker build --platform
  linux/amd64 -t x64-linux-dbus -f Dockerfile .`

## What was fixed in v1.19.36

Headline (full detail in `CLAUDE.md` § "Vivado License Manager + hw_ila
Drag"):

1. **ARGB32 component-alpha glyph rendering** (`.53` / `.54`) — License
   Manager text legibility, then full LCD-subpixel quality.
2. **`SubstructureRedirect` skip on `override_redirect=True`** (`.57`).
3. **`SetCloseDownMode(RetainPermanent)` honored** (`.58`) — windows
   survive their client's disconnect when requested.
4. **`postMotion` honors `ButtonMotionMask` + root-grab fallback**
   (`.62`) — the actual hw_ila drag fix.

Diagnostic infrastructure that survives the release:
- `[DRAG]` / `[ROUTE]` / `[DROP]` / raw-motion counter traces (opt-in
  `-DX11_TRACE_DRAG`).
- `[KEY_SEND]` logs `focus`, `modsRaw`, `state` for keyboard-side
  diagnosis (Debug builds only).

Release hygiene:
- High-frequency `TS_FPRINTF` calls converted to `TS_DBG` or wrapped
  with `#ifndef NDEBUG` so Release builds get a quiet stderr.
- Swift `[GEOM_NS] SIZE_HINTS_APPLIED` no longer prints a 309-digit
  decimal for unbounded `maxSize` — renders `MAX` instead.

## Open work for the next session

Ordered roughly by likely impact / ease.

### 1. `Ctrl+click → button 3` regression  (MEDIUM)

Ctrl+click no longer triggers button 3 in Vivado.  Two-finger trackpad
click still works as button 3.  Regression somewhere between v1.17 and
v1.19.  This is the user-facing item to chase next.

**Investigation plan**:
- `git log` on `SwiftX11/UI/Windows/X11WindowHost.swift` and adjacent
  files between v1.17 and v1.19 — the mouseDown / mouseUp handlers that
  decide which X11 button to emit.
- Sanity-check the current Ctrl+click path: in `X11WindowHost.swift`'s
  Cocoa mouse handlers, where do we look at `NSEvent.modifierFlags`?
  Compare to how Option+left-click correctly emits button 2 today.
- Likely cause: an earlier refactor introduced a branch that picks
  button 1 + Ctrl modifier instead of remapping to button 3 the way
  v1.9.6 originally implemented.

### 2. `PointerGrab::owner_fd`  (MEDIUM)

Quick-fix shipped in v.62 makes XDND root-grab motion fall back to
`drag_xid` when the grab window is root.  Cleaner long-term fix: add
`owner_fd` to `PointerGrab` (set in `GrabOps::handleGrabPointer`),
route motion events to that client directly via `sendEvent32` against
the live transport for that fd.  Eliminates the heuristic and handles
the case where a client root-grabs without an active drag.

Touchpoints:
- `include/Core/GrabTable.hpp` — add `int owner_fd = -1;` to `PointerGrab`.
- `src/Ops/GrabOps.cpp` line ~97 — pass `ctx.transport().clientFd()` into
  `setPointerGrab(...)`.
- `src/XProtoNotifyBridge.cpp` `postMotion` routing — when `haveGrab` and
  the grab is root (or any server-owned window), route delivery to
  `cs.transport()` for `activeGrab.owner_fd` instead of looking up the
  grab window's owner.

### 3. Vivado crashes after laptop sleep  (MEDIUM, long-running)

After leaving SwiftX11 + Vivado running overnight and sleeping the
laptop for several hours, the first click into Vivado on wake crashes
Vivado.  See `TODO.md` "Vivado Crashes After Laptop Sleep" for the
detailed signature and mitigation suggestions.  Hardest to repro
because of the time gate.

### 4. XI2 proper fix  (LOW)

XInputExtension is hidden (`present=0` in `QueryExtension`) because
enabling it crashes Electron.  Event format fixes are in place but
delivery has sequence-regression issues.  Per-client `xi2_root_mask`
tracking is needed before XI2 can be re-enabled.

### 5. Two-instance collision prevention  (LOW)

If SwiftX11 launches twice (e.g., user double-clicks app), both
instances try to bind to display :1 and fail in confusing ways.  Add a
launch check (either pid-file lock at `/tmp/.X11-unix/X1.lock` or
NSApp `applicationShouldHandleReopen` returning false to bring the
existing instance to front).

### 6. xterm `Ctrl+V` paste  (NOT A BUG — documentation work)

Confirmed not a SwiftX11 bug: xterm's `insert-selection` translation
lives on the VT100 widget and only fires when X11 focus is on that
widget; xterm doesn't call `XSetInputFocus` to put focus there, so the
translation never matches `Ctrl<Key>v` events delivered to the shell.

User workarounds (already in CLAUDE.md "Known Issues"):
- Option (⌥) + left-click on Magic Mouse → emits button 2 → pastes
  PRIMARY.
- Shift+Insert pastes PRIMARY.
- Add to `~/.Xresources`:
  ```
  XTerm*translations: #override \n\
    Ctrl<Key>v: insert-selection(CLIPBOARD,PRIMARY)
  ```
  Then `xrdb -merge ~/.Xresources` and relaunch xterm.

This belongs in user-facing docs (README / help menu); the help page
in the app already mentions clipboard-related quirks but does not yet
spell out the xterm-Ctrl+V workaround.

## Diagnostic flags worth knowing

To enable per-build, add `-D<FLAG>` to `OTHER_CPLUSPLUSFLAGS` in Xcode
build settings:

| Flag | What lights up |
|---|---|
| `X11_TRACE_VERBOSE` | All categories — extremely noisy |
| `X11_TRACE_DRAG` | `[DRAG]` BEGIN / motion / END / DROP / ROUTE / HOST_CORR lines for any button-down→button-up bracket |
| `X11_TRACE_FONT` | `[FontTable]` lookup details + `[LABEL]` text-render diagnostics |
| `X11_TRACE_RENDER` | RENDER extension per-op details (CreatePicture, AddGlyphs, CompositeGlyphs) |
| `X11_TRACE_LIFECYCLE` | `[LIFECYCLE]` window CreateWindow / MapWindow / Map subwindows / Expose |
| `X11_TRACE_PRESENT` | `[COPY_SURFACE]`, `[DAMAGE]`, `[BG_FILL]`, `[SNAPSHOT]` |
| `X11_TRACE_INPUT` | `[BTN_GRAB]`, `[DRAG_MOTION]` low-level input |
| `X11_TRACE_WIRE` | Every outgoing 32-byte wire packet — hundreds per session |

The Settings → Debug "Wire Trace" toggle is a runtime equivalent of
`X11_TRACE_WIRE` without rebuilding.

## Full documentation

- **Architecture**: `docs/CLAUDE.md` (also symlinked from `macos/CLAUDE.md`)
- **Roadmap & known issues**: `docs/TODO.md`
- **User-facing README**: `docs/README.md`
- **License**: `docs/LICENSE` (GPL-3.0)
- **GitHub releases**: https://github.com/LawrenceGibbons/SwiftX11/releases
