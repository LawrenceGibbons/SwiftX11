# Remediation Plan — Adversarial Review 2026-08-31

Companion to `ADVERSARIAL_REVIEW_2026-08-31.md` (section references `§N.M`
below point there). Plan drafted 2026-09-01 against `develop` @ v1.19.36.7-dbg.

**Verification status:** all five spot-checked Phase-0 claims confirmed in
code (XTEST 2.2 reply, QueryPointer raw mask, crossing ev[31], AllocColor
pixel-0 fallback, WindowClose seq=0). The review's cited line numbers and
mechanisms check out; treat its CONFIRMED findings as trustworthy, re-verify
PLAUSIBLE ones before acting.

---

## Guiding decisions

1. **Order by user pain, not by severity label.** Vivado/hw_ila/dialog
   correctness first; wire-protocol surgery (riskiest) only after its
   prerequisites; extension honesty and lifecycle hygiene follow.
2. **The floor stays until its feeders are fixed** (review §8 Phase 3 order):
   deleting the sequence floor before fixing seq-0 emitters and the broken
   instrumentation would convert masked bugs into live desyncs.
3. **One architectural keystone**: per-(window, client) event-mask tracking.
   Three finding clusters (XI2 §5.1, PropertyNotify §6.8, SendEvent mask
   semantics) all require it. Build once, in its own milestone (M6), after
   the quick wins — everything downstream (XI2 re-enable, multi-JVM
   correctness) sits on it.
4. **Every milestone = one dbg build + a Vivado smoke test** (launch, dialog
   open/close, tcl console, hw_ila drag, vlm scroll). Version bump each
   build per house convention.
5. **Don't touch the verified-good list** (review §9).

---

## M0 — Hotfix batch (v1.19.36.8) — one sitting

All one-to-five-liners, independently safe, one build:

| Fix | Ref | Note |
|---|---|---|
| XTEST GetVersion → 2.0 | §0 | Restores the documented GrabControl-crash workaround; docs/code re-converge |
| QueryPointer mask via `toX11State()` (both reply paths incl. host==0) | §2.1 | Directly affects Java drag loops / MouseInfo |
| Crossing ev[31] = `0x02 \| focus` | §2.3 | XI2 sender already correct — copy its logic |
| AllocColor: don't treat pixel 0 as failure | §4.5 | Black is a valid TrueColor pixel |
| WindowClose Unmap/DestroyNotify: stamp `lastSeq()` | §1.3 | Last zero-seq emitter; de-arms §1.2 poisoning |
| CompositeGlyphs len==0 → `continue` | §4.9 | Empty elts carry large deltas |
| Clamp BIG-REQUESTS ext length to advertised 4MB | §1.6 | Reject with BadLength instead of 17GB `resize()` |
| `_NET_FRAME_EXTENTS` type=CARDINAL | §3.6/§6.2 | One constant |
| Rescue ConfigureNotify: set SendEvent bit (0x80) | §3.6 | ICCCM synthetic-event marker |

Smoke focus: hw_ila drag (QueryPointer fix), dialog open/close, popup close
(WindowClose fix), text colors (AllocColor).

## M1 — Small-dialog closure (v1.19.36.9–.11)

The six confirmed mechanisms, review §3, in its ranked order:

1. WM_NORMAL_HINTS must never resize a mapped window; per-axis grow-only for
   unmapped tiny ones; synthetic ConfigureNotify whenever the server resizes.
2. Re-expose on any surface growth outside genuine Cocoa live-resize; reset
   the one-shot `surface_resize_exposed` on unmap.
3. Cocoa echo path: fix suppression-budget walk-back; wire up or delete the
   orphaned `shouldSuppressRootlessResize`.
4. `flushPendingMaps` on deadline/sync boundary instead of first socket
   drain; keep peak tracking alive while `wid ∈ pending_maps_`.
5. Clear peak/pending/post-map-notify state on unmap/destroy (also blocks
   ghost-map of already-unmapped windows).
6. Expose-after-Configure ordering + the supporting-defect list (§3 tail) as
   a cleanup pass.

Each item is separately testable; land 1–2 per build. This closes the
longest-running user-visible bug family in the project.

## M2 — Input correctness for Vivado (v1.19.36.12–.15)

Review §8 Phase 2, plus items already on TODO:

- Keyboard grab routing in the Key host-command path (§2.2 — Swing menus).
- Grab ownership/time/status: `client_fd` + `grab_time` on PointerGrab,
  AlreadyGrabbed returns, ungrab-by-owner-only (§2.5; subsumes the
  existing `PointerGrab::owner_fd` TODO item).
- Implicit grab: `drag_xid = deliver` + `grabWantsMotion()` filter (§2.6).
- Focus revert-to semantics; stop resetting focus to None on destroy (§2.9).
- TranslateCoordinates child field via the QueryPointer deepest-pick (§2.10).
- Ctrl+click state machine rewrite (§2.11 — the known regression; decide
  button identity once per physical press, symmetric down/up).
- do_not_propagate_mask enforcement (§2.8).
- Timestamp coherence: sample once per host command (§2.13); non-zero
  SelectionNotify/Clear times.

## M3 — Wire integrity endgame (v1.19.36.16+, gated)

Strict order (§8 Phase 3): extend the reply safety net to extension opcodes
(§1.7) → single structured `writeReply(header, payload)` entry point owning
lenw (§1.5) → fix the [SEQ_REGRESS]/ring-buffer instrumentation (§1.4) →
**then** delete the floor + SEQ_WRAP + payload heuristics (§1.1/1.2).
Add the §1.2 diagnostic log (max_wire_seq_ vs last_request_seq_ at SEQ_WRAP)
in M0-era builds so we have field evidence before the deletion. Test: sleep/
wake cycles with Vivado attached, the historical post-sleep crash repro.
Also here: head-of-line/EAGAIN backpressure handling (§1.8).

## M4 — Extension honesty (v1.19.37 line)

- Unadvertise DAMAGE and Composite until real (§5) — or implement DamageNotify
  minimally if Vivado needs it (check first with QueryExtension trace).
- XFIXES → 1.0; implement XFixesSelectionNotify off the existing SelectionOps
  owner tracking (§5).
- XTEST FakeInput → wire into the existing host-command input path (machinery
  exists; enables Robot/xdotool) or stop advertising.
- RANDR: rotation=1, track RRSelectInput, monotonic timestamps, answer
  GetScreenInfo/SetScreenConfig (§5).
- Delete the eight dead stub files in `src/Extensions/` (§5 housekeeping).
- GraphicsExpose for clamped/occluded CopyArea (§4.3) lands here too.

## M5 — RENDER/drawing fidelity (parallel-friendly with M4)

- PutImage ZPixmap→pixmap support (§4.1 — biggest rendering lie; Java2D AA).
- SetPictureTransform/SetPictureFilter honored for scale at least (§4.2).
- GetImage: BadMatch on out-of-bounds instead of short reply (§4.4).
- Trapezoids: source promotion + gradient sampling reuse from Composite (§4.6).
- Alpha-forcing only for window destinations; format-aware sampling (§4.7).
- RENDER cursors: create real cursors from pictures (§4.8).
- Remaining §4.10–4.12 items as a cleanup pass.

## M6 — Per-(window, client) event masks (architectural keystone)

Replace single `event_mask`/`owner_fd` delivery with a per-(window, client)
selection table consulted by all senders (§2.8/§6.8 PropertyNotify to all
selectors, SendEvent event-mask destinations, ChangeWindowAttributes no
longer clobbering other clients' masks). Then the XI2 re-enable path (§5
items 1–5) on top. Exit test: Electron/Vitis with XI2 advertised, two-JVM
clipboard, portal-gtk dialogs.

## M7 — Long-session hygiene

- rid_base slot recycling + ID-range enforcement on all Create* ops (§6.1).
- Resource free on disconnect: call the existing eraseOwnedBy family for
  pixmaps/GCs/cursors/fonts (§6.4).
- Selection-owner sweep on death/destroy (§6.3) + same-connection
  ConvertSelection fix (§6.10 — likely relevant to the open tcl-console
  shift-click copy bug).
- Grab cleanup on client death incl. RetainPermanent path (§6.5).
- Recursive DestroyWindow + property/peak/suppression purge (§6.7).
- KillClient + RetainTemporary semantics; allow children under retained
  windows (§6.6 — XDND robustness).

---

## Standing docs corrections (fold into M0 commit)

- CLAUDE.md "Clipboard Bridge (v1.19.35)" still says 4MB/16MB — now
  12MB/48MB (v1.19.36.1).
- After the M0 XTEST fix, the v1.19.14 "safe mode" claim becomes true again;
  add a note that it was found absent 2026-08-31 and restored.
- Note in CLAUDE.md that `src/Extensions/*.cpp` are dead until M4 removes them.

## Field observations (2026-09-01, testing v1.19.36.8)

- **tcl console copy of LARGE selections fails** → root-caused same day:
  Java uses INCR above its size threshold; our proactive capture had no
  INCR receive. Fixed in v1.19.36.9. (Selection size, not shift-click vs
  drag, was the discriminator.)
- **vlm column reorder drag doesn't work** (width-resize drag does) —
  likely M2 implicit-grab/drag territory (§2.6); retest after M2.
- **Tooltip text missing on first appearances**, starts working after
  opening a menu — popup first-paint timing; fold into M1 testing (§3.2
  surface-realloc re-expose is a candidate mechanism).
- **Status popups sometimes appear at screen upper-left** — matches the §3
  supporting defect "PPosition with obsolete-zero fields moves windows to
  (0,0)"; M1.
- Dialogs with buttons now center correctly (M0 `_NET_FRAME_EXTENTS` /
  synthetic ConfigureNotify fixes in effect).

## Open investigations folded into this plan

- **tcl console shift-click copy** (pre-review open item): first check §6.10
  same-connection ConvertSelection refusal and §2.13 zero-time
  SelectionNotify — both are plausible causes; the v1.19.36.1 capture traces
  remain in place for the repro.
- **Vivado post-sleep crash**: diagnostic logging lands in M0; the fix is the
  M3 floor removal.
