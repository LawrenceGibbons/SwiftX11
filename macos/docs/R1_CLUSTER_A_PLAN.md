# R1 — Root window as a real window (Cluster A) — implementation plan

Status: **Phase 1 implemented** — 1a/1b no-op rename (`dc68da9`) + 1c root-as-real-window (`5b5157d`), v2.0.0.1-dbg on develop, protocol gate PASSED 2026-09-09 — r1_test all green (handshake; root=0x2 as a real 4288x1967 IsViewable window; top-levels parent to 2; PointerRoot(1) vs root-window(2) focus distinct; xwininfo/xprop -root correct); app-level sanity (xterm/Vivado/Vitis) per user. **Phase 2 implemented** (A2 guards removed at all 11 SubstructureNotify sites + Cocoa-driven ConfigureNotify to the parent + root geometry refresh on ScreenLayoutChanged), v2.0.0.2-dbg — runtime gate PASSED 2026-09-09 (r1_p2_test 5/5; xev -root sees a real xterm’s Create/Map/Configure). v2.0.0.3-dbg adds the xorg DeleteWindow semantics the gate exposed: UnmapNotify before DestroyNotify for a mapped window (DestroyWindow/DestroySubwindows) and Unmap/DestroyNotify on client disconnect (was silent — root observers never learned a client died); r1_p2b_test + r1_p2_test destroy-while-mapped + xev -root/SIGTERM PASSED 2026-09-09. **Phase 3 implemented** (v2.0.0.4-dbg; .5 fixed the owner_fd gate in wantsBtn/wantsKey/wantsButton/sendFocusEvent that refused the root) — **gate PASSED 2026-09-09 on v2.0.0.5-dbg** (r1_p3_test 11/11: core Motion/Button/Key(PointerRoot) with event=root child=actor; XI_ButtonPress at an XI2 root selector with the core twin suppressed; XIGetSelectedEvents(root); RawMotion; core restored; Phase 1/2 regressions and r2_test clean): A4 — the button and key walks climb through the toplevel to root (pick_motion_target already did); A5 — XI2 root selections are root’s WindowTable entries (InputState side table and the deliverXI2 root fallback deleted; RawMotion/raw-button/raw-key gate on root’s xi2_mask); Enter/Leave to root emitted (EnterLeave emitOne no longer skips root); XI2 events carry root=2 (was the literal 1). Gate: r1_p3_test. Phase 4 not started. Design written overnight after R2 verified (develop @ v1.20.0.41-dbg, checkpoint branch `r2-verified`). R1 is pervasive (108 `kRootXid` references across 16 files), interdependent, and — because the root XID is fundamental to the connection handshake — a single miscategorised site breaks *every* client. It must be built in phases with a runtime test (xterm/Vivado/Vitis connect + input) after each. Do not merge blind.

**R1 COMPLETE (2026-09-10):** all four phases implemented and verified. Phases 1–3 (the "real root window core") gate-passed on v2.0.0.5-dbg (branch `r1-phase3-verified`, 1b4ac2b). Phase 4 (SendEvent-to-root A1 + EWMH ClientMessage interpretation) landed in v2.0.0.6 (inc 1: advertisement + `_NET_ACTIVE_WINDOW`), .7 (inc 2: `_NET_WM_STATE` maximize/fullscreen + `WM_CHANGE_STATE` iconify) and .8 (clipboard-regression fix: the SendEvent-to-root block must not swallow the SelectionNotify capture whose requestor is root — Vivado Edit→Copy), verified 2026-09-10 (checkpoint `r1-complete`). develop stays DEBUG-build (2.0.0.8-dbg, DEBUG_BUILD unflipped); the v2.0.0 release is the user's call. Note: `r1_p3_test` is position-dependent and flakes when other clients (Vivado) are open — the delivery code is unchanged since .5's clean 11/11; take a deterministic Phase-3 baseline with no other clients running.

Source: `docs/XORG_COMPARISON_REVIEW_2026-09-08.md` §1 (Cluster A, A1–A5), §11 R1.

## 1. The core problem: one value means three things

`XConstants.hpp:12` defines `kRootXid = 0x00000001`. Wire value **1** is overloaded:

1. **The root window's identity** (parent of top-levels; target of `xprop -root`, root SubstructureNotify, root selections, EWMH ClientMessages).
2. **The `InputFocus` SendEvent sentinel** (`dix/events.c`: SendEvent destination 1 = "the focus window"; 0 = "the pointer window").
3. **The `PointerRoot` focus sentinel** (SetInputFocus/GetInputFocus focus 1 = "focus follows the pointer"). Internally we store this as `focus_xid == kRootXid`.

Because all three are the literal `1`, a client's `SendEvent(dest=root)` is rewritten to the focus window (A1), and internal `focus_xid == kRootXid` cannot distinguish "PointerRoot" from "focus is the root window." X11 keeps these separate: the root is a real server-space XID (never 0 or 1), while 0/1 are wire sentinels.

**R1 = split the three meanings apart, give the root a real XID with a real `WindowView`, and route root-targeted traffic through the normal machinery.**

## 2. New constants (XConstants.hpp)

```cpp
static constexpr uint32_t kRootWindowXid   = 0x00000002u; // real root window identity (server space, >1, outside client rid ranges: 0x20/0x21 are cmap/visual, so 0x02 is free)
static constexpr uint32_t kPointerRootFocus= 0x00000001u; // internal focus sentinel: focus follows the pointer (wire PointerRoot)
// Wire sentinels stay literals at their handlers:  SendEvent dest 0=PointerWindow / 1=InputFocus;  SetInputFocus focus 0=None / 1=PointerRoot.
// Keep `kRootXid` as a DEPRECATED alias during migration? NO — remove it, so every one of the 108 sites is forced through the audit (§4). A leftover alias is how a miscategorised site hides.
```

Rationale for removing `kRootXid` entirely rather than redefining it: the compiler then flags all 108 sites, and each is consciously reclassified. A silent redefinition (`kRootXid = 2`) would leave the PointerRoot-sentinel sites (which must stay `== 1`/`kPointerRootFocus`) wrongly pointing at the root window.

## 3. Root WindowView

At server init (once, in `XProtoServer`/`WindowTable` bring-up — same place the ID space is set up), insert a `WindowState` for `kRootWindowXid`:
- `mapped = true`, `override_redirect = false`, `parent_xid = 0` (root has no parent; the climb terminates here).
- `x=0, y=0, w/h = getScreenLayout().virtual_w/h` (kept in sync on `ScreenLayoutChanged`).
- `owner_fd = -1` (server-owned; never destroyed, never reassigned, exempt from `eraseOwnedBy`).
- `event_mask = 0`, `xi2_mask = 0` initially; per-client selections on root land in the normal `client_masks` / XI2 per-client structures (which already exist post-R2 §C: `WindowTable` XI2ClientMask, and `InputState::xi2_root_sels` folds in — see §5).
- depth/visual/colormap = the root's advertised values.

**Ripple to audit:** many sites do `if (xid != kRootXid && !snapshot(xid)) BadWindow`. Once `snapshot(kRootWindowXid)` succeeds, those become plain `if (!snapshot(xid)) …` — but verify each didn't rely on root's absence for other logic (e.g., QueryTree already special-cases root children via `children_order_`).

## 4. The 108-site audit (per file), by category

Each `kRootXid` use is one of: **[W]** root-window identity → `kRootWindowXid`; **[PR]** PointerRoot focus sentinel → `kPointerRootFocus` (compare `focus_xid`); **[WIRE]** a raw wire sentinel already written as literal `0`/`1` (leave). Counts from `grep -rn kRootXid`:

| File | refs | expected category | notes |
|------|-----:|-------------------|-------|
| `Ops/QueryOps.cpp` | 21 | mixed [W]+[PR] | SetInputFocus/GetInputFocus: `focus==1`→PointerRoot **[PR]**; QueryTree/QueryPointer root, TranslateCoordinates root, GetGeometry root **[W]**. This file is the crux of the [PR] vs [W] split. |
| `Ops/WindowOps.cpp` | 15 | [W] | CreateWindow parent==root, the A2 `parentMask` guards (547/617/618), Reparent/Configure/Destroy notify to root. Remove guards → root SubstructureNotify via `sendEventToSelectors(kRootWindowXid, …)`. |
| `Ops/ExtensionOps.cpp` | 14 | [W] | XI2 root queries, `childrenInStackOrder(kRootXid)`, XTEST motion host lookup (added R2). |
| `Utils/EnterLeave.hpp` | 12 | mixed | crossing chains: root as the common ancestor top **[W]**; `isAncestor`/`ancestorChain` terminate at root. Check none mean PointerRoot. |
| `XProtoServerBridge.cpp` | 9 | [W] | Button/Key host-cmd host==root, `childrenInStackOrder(1)`, effectiveHost. Key path `focus == kRootXid` → **[PR]** (PointerRoot delivery walk, M19) — this one is [PR], not [W]! |
| `Utils/FocusEvents.hpp` | 9 | [PR] mostly | DoFocusEvents None/PointerRoot handling — these are focus sentinels **[PR]**. |
| `Ops/GrabOps.cpp` | 6 | [W] | grab-window validation allows root; keyboard-grab focus pair `from` may be PointerRoot **[PR]** — audit each. |
| `Transport/XProtoDaemon.cpp` | 4 | [W] | cross-client/selector delivery; the stale-grab guard. |
| `Ops/PropOps.cpp` | 4 | [W] | root property ops (`_XKB_RULES_NAMES`, EWMH on root). |
| `Core/WindowTable.cpp` | 4 | [W] | `childrenInStackOrder(root)`, `topLevelAncestorOf` termination, QueryTree root. |
| `Ops/EventOps.cpp` | 3 | [W]+[PR] | A5 root XI2 fallback (remove — §5); focus event root. |
| `Ops/WindowAttrOps.cpp` | 2 | [W] | ChangeWindowAttributes on root (A2 site 567). |
| `Utils/GrabChoreography.hpp` | 2 | [PR]/[W] | keyboard grab focus pair endpoints. |
| `Ops/ColorOps.cpp` | 1 | [W] | default colormap/root. |
| `Extensions/XKBOps.cpp` | 1 | [W] | `_XKB_RULES_NAMES` set on root. |
| `Core/XConstants.hpp` | 1 | (def) | the definition itself. |

The **[PR] sites are the dangerous ones** — they must NOT become `kRootWindowXid`. Known [PR] sites to get right: `QueryOps` SetInputFocus (`newFocus = (focus==1)?PointerRoot:focus`) and GetInputFocus reply; `XProtoServerBridge` Key path `focus == kRootXid` PointerRoot branch (M19); `FocusEvents.hpp` None/PointerRoot; keyboard-grab focus pairs. Everything selecting/climbing/parenting is [W].

Note GetInputFocus currently reports `focus_xid` and, because our root XID *is* the wire PointerRoot value, "PointerRoot" already serialises correctly (review §v1.20.0.37 note). After the split, GetInputFocus must map internal `kPointerRootFocus` → wire `1` and a real root-window focus → `kRootWindowXid`.

## 5. Delivery / selection routing (A3, A4, A5)

- **A3** (XFixesSelectionNotify on root) — already fixed in R0 (v1.20.0.39: deliver to the subscriber fd). Re-confirm it works once root is a real window; no longer a special case.
- **A4** (core selections on root): the `SelectInput`/property/selection climb walks (`XProtoServerBridge.cpp:935-948`, `XProtoNotifyBridge.cpp:497-504`) must continue past the toplevel to `kRootWindowXid` as the final ancestor. With top-levels' `parent_xid == kRootWindowXid` and a real root WindowView, the existing walk-up terminates at root and consults its `client_masks`. Verify termination (root.parent_xid==0 stops the loop).
- **A5** (XI2 root selections in the walk): today `InputState::xi2_root_sels` is a side table consulted by a post-hoc fallback in `deliverXI2` (theft/starvation). With root a real window, fold root XI2 selections into the normal `xi2SelectorsOf` walk (root is the ordinary last window, `dix/events.c:2865-2900`) and delete the fallback. Keep `xi2_root_masks` only if RawMotion fan-out (window-free, root-level) still needs it — RawMotion is not window-targeted, so it stays a root-selector broadcast either way; reconcile the two so a root selection is stored once.
- **A1/SendEvent-to-root**: `SelectionOps.cpp:754-757` dest 0/1 stay sentinels; `dest == kRootWindowXid` now falls through to `sendEvent32(kRootWindowXid, …)` which resolves via root's WindowView + selectors. No collision because root != 1.

## 6. Phased rollout (test after each; revert per phase; checkpoint = `r2-verified`)

- **Phase 1 — the split (foundational).** New constants; remove `kRootXid`; reclassify all 108 sites ([W]→`kRootWindowXid`, [PR]→`kPointerRootFocus`); add the root WindowView; setup `root_xid = kRootWindowXid`. **Test:** every client still connects (this is the make-or-break); `xwininfo -root`, `xprop -root`, QueryTree, QueryPointer, xterm/Vivado/Vitis input all unchanged. If any client fails to connect, a [W]/[PR] site is miscategorised.
- **Phase 2 — A2 guards.** Remove the `parent == kRootXid ? 0 : …` masks; deliver top-level Create/Map/Unmap/Destroy/ConfigureNotify to root SubstructureNotify selectors. **Test:** `xev -root` / `wmctrl -l` / `xdotool search` see top-levels appear/disappear; Java XToolkit root observer.
- **Phase 3 — A4/A5 root selections.** Root in the propagation + XI2 walks; delete the `deliverXI2` fallback. **Test:** `xev -root` selection events; a second XI2 root selector alongside xeyes (the R2 §C test) still fans out; no theft/starvation.
- **Phase 4 — SendEvent-to-root + EWMH ClientMessage (follow-on).** Interpret `_NET_WM_STATE` (maximize/fullscreen of mapped windows), `_NET_ACTIVE_WINDOW`, `WM_CHANGE_STATE` (iconify), XDND root-proxy. **Test:** Java maximize/iconify of an already-mapped Vivado window; XDND across apps.

Phases 1–3 are the "real root window" core; Phase 4 is the EWMH payoff and can be its own release increment.

## 7. Version / release interaction (READ FIRST — see the overnight note)

R1 is the **1.21.0** line (user's call: major bump for R1). But note the release-numbering conflict found overnight: **v1.20.0 is already a published GitHub release** (2026-09-03), and develop's R2 work is 70 unreleased commits mislabeled `1.20.0.N-dbg`. Sort out the R2 release number (likely **v1.20.1**) BEFORE cutting R1's 1.21.0, so the sequence is v1.20.0 (shipped) → v1.20.1 (R2) → v1.21.0 (R1). Bump `SWIFTX11_VERSION_BASE` to `1.21.0`, `SWIFTX11_DEBUG_BUILD = 1` when Phase 1 code starts.

## 8. Rollback

`r2-verified` branch = the verified R2 state (develop @ dbdae05). Each phase is a separate commit on the worktree; if a phase regresses, `git reset` to the prior phase or check out `r2-verified`. Do not merge any phase to develop until it passes its runtime test.
