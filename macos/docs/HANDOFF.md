# SwiftX11 — Handoff (v2.0.0)

## Context

SwiftX11 is a native macOS X11 protocol server (Swift UI + C++ protocol core).
It implements the X11 wire protocol — 100+ core opcodes and 12 advertised
extensions — so X11 clients render into native Cocoa/Metal windows with no
XQuartz dependency. It runs xterm, xeyes, xcalc, xclock, xfd, Xilinx Vivado
2024.1 / 2025.1 (Java Swing, incl. License Manager and hw_ila waveform debug)
and Xilinx Vitis 2025.1 (Electron/GTK) from an AlmaLinux 9 Docker container.

The current release is **`v2.0.0`** (tagged on `main`, shipped 2026-09-16 —
https://github.com/LawrenceGibbons/SwiftX11/releases/tag/v2.0.0). It made the
X11 **root window a real window on the wire** (its advertised XID changed from
`1` to `0x2` — the wire change behind the major bump) and completed a broad
protocol-conformance pass verified against the xorg-server source.

Documentation map:
- **Per-build changelog + architecture** — `docs/CLAUDE.md` (also the project
  `CLAUDE.md`). Exhaustive; internal review-batch labels (R0–R6, E1/E2, C3, the
  M/L items) are explained there.
- **Roadmap** — `docs/TODO.md`.
- **Developer backlog** (consciously-deferred work) — `docs/DEFERRED.md`.
- **User-facing limitations** — `docs/KNOWN_ISSUES.md`.
- **Release notes** — `docs/RELEASE_NOTES_v2.0.0.md`.
- **The reviews this release answers** — `docs/XORG_COMPARISON_REVIEW_2026-09-08.md`
  (R0–R5) and `docs/RELEASE_REVIEW_2026-09-16.md` (R6 + doc corrections); the XI2
  line-by-line audit is `docs/XI2_XORG_COMPARISON.md`.
- **Regression probes** — `macos/tests/probes/` (see below).

## Status snapshot (v2.0.0)

| Client | State |
|---|---|
| **xterm** (`-sb -rightbar -bc`) | ✅ cursor blink, scroll, Option+click paste, Shift+Insert |
| **xeyes** | ✅ SHAPE transparency, RawMotion pupil tracking via XI2 |
| **xcalc**, **xclock**, **xfd** | ✅ |
| **Vivado 2024.1 / 2025.1 main GUI** | ✅ multi-monitor, dialogs, JidePopup, IP gen, clipboard, hw_ila drag |
| **Vivado License Manager** | ✅ ARGB32 component-alpha glyphs |
| **Vitis 2025.1 (Electron/GTK)** | ✅ menus, dialogs, portal-GTK file dialogs (over XI2) |

XI2 (XInput2) and XKEYBOARD are **complete and default ON**. Extensions
advertised (12): BIG-REQUESTS, RENDER, XFIXES (1.0), RANDR, XINERAMA, Generic
Event, SHAPE, XC-MISC, XTEST, Composite, XInputExtension, XKEYBOARD. DAMAGE has
handlers but is deliberately **not** advertised (a rootless server emits no
DamageNotify).

## Development conventions

### Building
- **Build**: the user builds from Xcode (Cmd+B on the `SwiftX11` target). Do NOT
  run `xcodebuild` from the CLI during development — it can interfere with
  Xcode.app. (The release DMG script is the one sanctioned `xcodebuild` use; it
  uses a separate `build/DerivedData` so it doesn't collide.)
- **Version**: `X11LowLevel/include/SwiftX11Version.h`. Debug iterations bump
  `SWIFTX11_DEBUG_BUILD` (banner reads `vX.Y.Z.N-dbg`); a release sets
  `SWIFTX11_VERSION_BASE` and `SWIFTX11_DEBUG_BUILD = 0`. Bump the debug counter
  on every behavioral change so the startup banner distinguishes builds.
  **NOTE:** `develop` currently sits at `DEBUG_BUILD = 0` (the v2.0.0 release
  commit). The first dev change after the release should bump it back to `1`.

### Git workflow (worktree → develop → main)
- Claude Code edits in a **git worktree** under `.claude/worktrees/<branch>/`,
  commits there, then merges to `develop`:
  `cd /Users/lkg/Documents/Vivado/SwiftX11/macos && git merge <branch> --no-edit`.
- **CRITICAL:** never edit tracked files directly in the main checkout while
  Xcode has the project open — Xcode crashes if source changes under it. Always
  edit in the worktree. (Merging into `develop` is fine; the user rebuilds after.)
- **`develop` is where the user tests.** Leave `main` alone between releases.
  `main` moves only at a release — fast-forwarded to the release commit, then an
  annotated `vX.Y.Z` tag. Between releases `develop` runs ahead of `main`.
- Commit attribution used this project: `Co-Authored-By: Claude Opus 4.8
  <noreply@anthropic.com>`.

### Working discipline that paid off
- **Verify X11/XI2 semantics against the xorg-server source** (file:line), never
  by blaming the client — the same clients work on a real xorg (nxagent), so
  every divergence was ours. The whole R0–R6 campaign is xorg-cited on this basis.
- **Probes must mirror what real clients send.** The G4 InputOnly probe passed
  with a CopyFromParent visual but Vitis broke because GDK sends the parent's
  real visual — a happy-path probe that missed the real-client pattern.
- **On every verified step, update both** `docs/CLAUDE.md` (exhaustive) **and**
  the user-facing highlights; unverified work stays in CLAUDE.md only.

### Release process (as done for v2.0.0)
1. Flip `SWIFTX11_DEBUG_BUILD` to `0` in `SwiftX11Version.h`; commit as
   `release: vX.Y.Z` on the worktree, merge to `develop`.
2. `git branch -f main develop` (fast-forward), `git tag -a vX.Y.Z -m "…"`,
   `git push origin develop main vX.Y.Z`. (GitHub may print a transient
   `remote: fatal error in commit_refs`; verify with `git ls-remote origin` and
   a re-push that reports "Everything up-to-date" — the refs land.)
3. Build the DMG: `bash macos/scripts/build-dmg.sh` → `macos/build/SwiftX11-<ver>.dmg`
   (self-contained: app + bundled X11 fonts inside `.app/Contents/Resources/fonts`).
   **Ship the `.dmg`, not the `.pkg`** — the app bundles its own fonts, so the
   `.pkg`'s optional font-install component is redundant. (v1.21.0 shipped a
   `.pkg` as a one-off; v1.19.35–v1.20.0 and v2.0.0 use the DMG.)
4. `gh release create vX.Y.Z --draft --title "SwiftX11 vX.Y.Z" --notes-file <body> <dmg>`,
   review, then `gh release edit vX.Y.Z --draft=false --latest`.

### Regression probes — `macos/tests/probes/`
Standalone X11 client programs, one per protocol area, that assert
correct-vs-wrong behavior against a running server. **Build & run** (with
SwiftX11 running on display :1):
```
cc -o <name> <name>.c -I/opt/X11/include -L/opt/X11/lib -lX11 [-lXi -lXtst]
DISPLAY=127.0.0.1:1 ./<name>     # exit 0 = PASS
```
See `macos/tests/probes/README.md` for what each verifies and which are
"Vivado-safe" (map nothing) vs. which briefly map a window. **`r6_1_test` is
destructive on an unguarded build** — it calls `DestroyWindow(root)`, a no-op
only on v2.0.0+; never run it against a build without the R6.1 guard.

Only the R5/R6 probes were preserved (the R0–R4 probes lived in an ephemeral
scratchpad and were lost before this handoff). Their pass records are in
CLAUDE.md; re-create from the cited behavior if the R0–R4 paths regress.

### Docker container
- **Image**: `x64-linux-dbus` (AlmaLinux 9 + dbus + libxkbfile).
- **DISPLAY**: `host.docker.internal:1` (SwiftX11 on TCP port 6001).
- **dbus**: `dbus-daemon --session --fork --print-address` (not `dbus-launch` —
  avoids a stale X11 root-window property that breaks a second container launch
  in the same SwiftX11 session).
- **Vitis** needs `--disable-gpu` (Electron under Rosetta).
- **Rebuild after entrypoint edits**: `cd /Users/lkg/Documents/Vivado/vivado2023
  && docker build --platform linux/amd64 -t x64-linux-dbus -f Dockerfile .`
- Launch scripts: `~/Documents/Vivado/vivado2023/launch_vivado_sx11.sh`,
  `launch_vitis_sx11.sh`.

### Trace tiers
1. **Always-on** (Release too): version banner, `[X11]` listen/disconnect,
   `[X11_ERROR]`, extension/screen/font summaries.
2. **`#ifndef NDEBUG`** (Debug builds; Xcode Debug scheme): `[LIFECYCLE]`,
   `[BTN]`, `[GrabPointer]`, `[CLIPBOARD]`, `[GEOM_NS]`, etc. via `TS_DBG(...)`
   (`Utils/MachTime.hpp`), no-op under NDEBUG.
3. **Opt-in `-DX11_TRACE_<CAT>`** in `OTHER_CPLUSPLUSFLAGS`: `X11_TRACE_RENDER`,
   `X11_TRACE_DRAG`, `X11_TRACE_FONT`, `X11_TRACE_WIRE`, … (`X11_TRACE_VERBOSE`
   = all).
4. **Runtime UI toggles** — Settings → "Wire Trace (stderr)" and "Draw Trace"
   (`[DRAWSEQ]`, added for the JIDE-tab diagnosis).

## Open work for the next maintainer

The 2026-09-08 review (R0–R5) and 2026-09-16 review (R6) are **complete** and
shipped in v2.0.0. What remains is the review's **§2 "should-fix-soon" list**,
which ships as documented known issues and rides the v2.0.x train — full detail
in `docs/DEFERRED.md` and `docs/RELEASE_REVIEW_2026-09-16.md §2`. Highlights:

1. **Reparent/SaveSet Swift-layer integration** — cross-top-level `ReparentWindow`
   and the embedder-death SaveSet rescue update the X11 tree but not the macOS
   window layer, so such a window can end up permanently invisible. Rare (XEmbed);
   in `KNOWN_ISSUES.md`.
2. **Post-sleep crash** — SwiftX11/Vivado left overnight can crash on wake; a
   suspected contributing factor was removed in v2.0.0.19 but there is **no
   confirmed fix** and no post-sleep field confirmation yet. Listed in
   `KNOWN_ISSUES.md`; leave open until an overnight cycle passes clean.
3. **R6.4 hw_ila re-verify** — the RetainPermanent non-window-resource-leak fix
   is code-verified and general-Vivado-clean, but the specific hw_ila drag path
   (which sets RetainPermanent every drag) was not re-exercised. Confirm on the
   next hardware session.
4. **Two-instance collision** — launching SwiftX11 twice collides on display :1;
   no single-instance lock. In `KNOWN_ISSUES.md`.
5. Smaller items — GrabServer race, EWMH `WM_STATE`/`_NET_ACTIVE_WINDOW`
   bookkeeping, extension reply safety net (a truncated reply-bearing *extension*
   request hangs its client), RENDER polish, XFIXES/RANDR timestamp consistency,
   debug-diagnostics cleanup, XC-MISC XID reclamation. All in `DEFERRED.md` /
   review §2.

## What lives OUTSIDE this repo (relevant to an account/machine handoff)

- **Claude Code memory** — this project's working conventions (worktree edits,
  git workflow, verify-against-xorg, release-notes cadence, version bumps) lived
  in the previous account's personal memory. The durable ones are captured in
  this file and `docs/CLAUDE.md`; a new account starts with empty memory and
  should re-learn them from here. Machine-specific facts not in the repo: the
  OrbStack data drive (`xilinx_orb`, mounted via symlink) must be mounted before
  launching the Vivado container; a separate uHAL/IPbus Alma 9 container lives at
  `~/Documents/Vivado/uhal-alma9`.
- **GitHub identity** — v2.0.0 was pushed and released under the `LawrenceGibbons`
  GitHub account via `gh`. The remote is
  `https://github.com/LawrenceGibbons/SwiftX11.git`. A team account pushing under
  a different identity needs its own `gh auth` / push access to that repo (or a
  fork + transfer).
- **The DMG artifact** — `macos/build/` is git-ignored; the shipped
  `SwiftX11-2.0.0.dmg` is attached to the GitHub release, not stored in the repo.
