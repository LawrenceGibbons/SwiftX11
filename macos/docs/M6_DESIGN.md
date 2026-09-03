# M6 Design — Per-(window, client) Event Masks + XI2 Re-enable

Status: **DRAFT for approval** · 2026-09-02 · against develop @ v1.19.36.31-dbg
Companion to `REMEDIATION_PLAN.md` (M6) and `ADVERSARIAL_REVIEW_2026-08-31.md`.

---

## 1. Goal

Replace the single `event_mask` / `owner_fd` per window with a per-(window,
client) event-selection model, so that:

- Multiple clients can independently select events on the same window without
  clobbering each other (§2.8 / §6.8 — the multi-JVM and portal-gtk failures).
- "Broadcast" events (PropertyNotify, Structure/Substructure notify, Expose,
  Visibility, Colormap) reach **every** interested client, not just the window's
  owner.
- XI2 selection becomes per-client, which is the precondition for re-advertising
  `XInputExtension` (xeyes pupil tracking; the `[EXT_PROBE]` demand) without the
  Electron startup crash.

Non-goal: rewriting pointer/keyboard input routing. See §3 — the X11 spec makes
device-event selection **exclusive**, so single-target routing is already
correct for those; we deliberately leave M2's hard-won input path untouched.

---

## 2. Current model (verified against code, not memory)

Single value per window, one owning client:

- Storage: `WindowState.event_mask` (`WindowTable.hpp:192`), `xi2_mask`
  (`:193`), `owner_fd` (`:226`); snapshot copy in `WindowView`
  (`WindowView.hpp:28/29/56`).
- Set by: CreateWindow `upsert(...event_mask, owner_fd)`
  (`WindowOps.cpp:547`); ChangeWindowAttributes/SelectInput
  `setEventMask(wid, cur_mask)` (`WindowAttrOps.cpp:262`) — **overwrites the
  single mask**, so a second client clobbers the first.
- Delivery gates read the single mask (representative sites):
  ConfigureNotify/Expose `EventOps.cpp:349/365`, FocusEvent `:612`,
  PropertyNotify `PropOps.cpp:44`, Structure/Substructure notify throughout
  `WindowOps.cpp` (self = StructureNotify, parent = SubstructureNotify), button
  `XProtoServerBridge.cpp:829-837`, key `:1040-1048`, motion
  `XProtoNotifyBridge.cpp:290/471-478`.
- Routing: `sendEvent32`/`sendEventVariable` route to exactly one fd —
  `owner_fd` if it differs from the current client, via
  `XProtoDaemon::sendEventCrossClient` (`XProtoTransport.cpp:644`,
  `XProtoDaemon.cpp:232/257`). **There is no all-selectors broadcast primitive.**
- XI2: per-window `xi2_mask` OR a single global `InputState.xi2_root_mask`
  (`InputState.hpp:42`); set by XISelectEvents (`ExtensionOps.cpp:1259-1263`:
  `window==1` → global root mask, else per-window). Senders compute
  `eff = xi2_mask | xi2_root_mask` (`EventOps.cpp:702/750/795/829/873`).
  `XInputExtension` advertised `present=0` (`QueryOps.cpp:633`); handlers stay
  registered, so nothing dispatches only because clients never learn the opcode.

---

## 3. The spec insight that bounds the scope

X11 core events fall into two delivery classes:

**(A) Broadcast — delivered to every client whose per-window mask selects them.**
Expose, GraphicsExpose/NoExpose, VisibilityNotify, Structure notifies
(Circulate/Configure/Destroy/Gravity/Map/Reparent/Unmap) via StructureNotifyMask
on the window, the Substructure variants via SubstructureNotifyMask on the
parent, PropertyNotify, ColormapNotify. Multiple clients legitimately select
these on the same window (two AWT connections on root; a WM plus the app).

**(B) Exclusive — at most one client per window may select them.**
KeyPress/Release, ButtonPress/Release, Enter/LeaveNotify, PointerMotion (and the
Button*Motion / PointerMotionHint family), FocusChange, plus
SubstructureRedirect / ResizeRedirect / ButtonPress selection. The server returns
**BadAccess** if a second client tries to select an already-selected exclusive
mask. Because only one client can own these, single-target routing (what we have)
is already correct — the only gap is enforcing the exclusivity, which is optional
and low-value for our clients.

**Consequence:** M6 only needs multi-client delivery for class (A). Class (B)
keeps the existing owner/selector routing. This is what keeps the refactor
tractable and keeps the delicate M2 input path out of scope.

---

## 4. Proposed data model

Add a per-window interest list; keep the existing fields as a derived cache so
the ~40 existing `event_mask &` gate sites keep working unchanged.

```cpp
// WindowTable.hpp, inside WindowState:
struct ClientMask { int fd; uint32_t mask; };
std::vector<ClientMask> client_masks;   // per-(window,client) selections
// event_mask stays, now DERIVED = OR of all client_masks[].mask (the "does any
// client want this?" cache).  owner_fd stays (creator; class-B routing).
```

Rules:
- `setEventMask(wid, fd, mask)` (new signature) updates *this fd's* entry
  (insert / replace / erase-on-0), then recomputes `event_mask` as the union.
- CreateWindow seeds `client_masks = {{owner_fd, initial_mask}}`.
- On client disconnect, drop that fd's entries from every window's list and
  recompute unions (hook in `removeClient`, alongside the existing sweeps).
- WindowView carries a small copy for read paths; broadcast delivery reads the
  authoritative list under the WindowTable lock via a new accessor (no copy of
  the vector into every snapshot — only the union is snapshotted).

Exclusivity (class B): **not enforced in Stage 1** (no BadAccess); last selector
of an exclusive bit wins, matching today's behavior. Tracked as an optional
Stage 3 item so we don't risk regressing an app that double-selects benignly.

---

## 5. New delivery primitive

```cpp
// XProtoDaemon: send a 32-byte event to every client selecting `bit` on `wid`,
// restamping sequence per target (reuses sendEventCrossClient's restamp logic).
bool sendEventToSelectors(uint32_t wid, uint32_t bit, const uint8_t ev[32]);
```

- Reads `wid`'s `client_masks`, and for each `{fd, mask}` with `mask & bit`,
  looks up the session (`findClient(fd)`) and writes the event with that
  client's `lastSeq()` (identical to the existing cross-client path, just
  iterated).
- Class-A senders switch from `sendEvent32(wid, ev)` to
  `sendEventToSelectors(wid, <bit>, ev)`. For Substructure notifies the target is
  the parent and the bit is SubstructureNotify.
- Class-B senders, Selection*, ClientMessage, Focus (directed) keep
  `sendEvent32` unchanged.

---

## 6. Staged delivery (each stage = its own dbg build + test gate)

### Stage 1 — per-client masks + class-A broadcast (XI2 stays hidden)
Storage (§4) + primitive (§5), then convert the class-A senders:
- PropertyNotify → `sendEventToSelectors(wid, PropertyChange, ev)`
  (`PropOps.cpp:44`) — fixes §6.8.
- Structure notifies to self → selectors on `wid` with StructureNotify.
- Substructure notifies to parent → selectors on parent with SubstructureNotify
  (Create/Destroy/Map/Unmap/Reparent/Circulate in `WindowOps.cpp`;
  ConfigureNotify-to-parent in `WindowAttrOps.cpp:544-558`).
- Expose/Visibility as they arise.
- ChangeWindowAttributes/SelectInput stops clobbering (§4).
- **No XI2 exposure.** Risk is contained to notify delivery.
- Exit test: two-JVM clipboard, portal-gtk dialogs, Vivado dialog open/close,
  xterm/xcalc/xeyes basics, hw_ila drag. Everything M2/M7 tested must still pass.

### Stage 2 — XI2 per-client + re-advertise (its own build, reversible)
- Replace global `xi2_root_mask` with a per-client root list (mirror of §4 for
  XI2) and a per-(window,client) XI2 mask; XI2 senders iterate selectors.
- Flip `XInputExtension` to `present=1` (`QueryOps.cpp`) + list it.
- Turn on **Wire Trace** during Vitis startup to catch the historical sequence
  regression *before* it crashes (the GenericEvent variable-length delivery path
  is the prime suspect per CLAUDE.md).
- **Rollback is a one-line flip back to `present=0`**: the per-client XI2
  infrastructure stays, dormant, exactly as today — so a failed Stage 2 leaves us
  no worse than the current shipping state.
- Exit test: Electron/Vitis launches and stays up with XI2 advertised; xeyes
  pupil tracking follows the cursor; menus/dialogs still interactive.

### Stage 3 — optional spec-compliance follow-ups (only if a case demands)
- Exclusivity BadAccess for class-B masks (§3).
- Device-event routing to the actual selector fd when selector ≠ creator.
- SendEvent event-mask destination semantics (§2.8 tail).

---

## 7. Risk register

| Risk | Likelihood | Mitigation |
|---|---|---|
| Broadcast delivers duplicate/incorrect events, breaking a working notify path | Med | Union cache keeps existing gates identical; only the *send* fans out. Diff-test against current single-owner behavior for the single-client case (must be byte-identical). |
| XI2 re-enable re-crashes Electron | **High** | Stage 2 isolated; wire-trace first; one-line rollback to present=0 with infra intact. |
| Per-client list not cleaned on disconnect → stale fd send | Med | Explicit sweep in `removeClient`; `sendEventToSelectors` skips fds `findClient` can't resolve. |
| Lock ordering (WindowTable list read during send) | Med | Snapshot the `{fd,mask}` list under the table lock, release, then send — never hold the table lock across a socket write. |
| Perf: iterating selectors on hot notify paths | Low | Lists are tiny (1–3 entries); union early-out unchanged. |

---

## 8. Test / exit criteria

- **Regression floor:** everything on the verified-good list (xterm, xcalc,
  xeyes, Vivado main+dialogs, vlm scroll, hw_ila drag, clipboard round-trip)
  passes after Stage 1 and again after Stage 2.
- **Stage 1 proves:** PropertyNotify reaches multiple selectors (multi-JVM
  clipboard), portal-gtk dialogs unaffected, no notify regressions.
- **Stage 2 proves:** XI2 advertised + Vitis stable + xeyes tracking; else
  rollback and keep Stage 1.

## 9. Rollback

- Stage 1: revert the milestone commits; `develop` returns to
  `checkpoint-1.19.36.31` behavior. A fresh checkpoint branch is cut before
  Stage 1 begins.
- Stage 2: flip `present=0` — no code revert required.

---

## 10. Open questions for approval

1. **Scope of Stage 1 class-A conversion** — do all of PropertyNotify +
   Structure/Substructure + Expose in one build, or land PropertyNotify (the
   clearest win) first and the notify family second?
2. **Exclusivity (BadAccess)** — leave as Stage 3 optional (recommended), or
   enforce in Stage 1 for correctness?
3. **XI2 Stage 2 timing** — attempt in this M6 pass, or land Stage 1, ship the
   next release, and treat XI2 re-enable as a separate dedicated effort given its
   High risk?
