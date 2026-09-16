# SwiftX11 — Deferred / backlog

Developer-facing tracker of work we consciously did **not** do, plus xorg-review
items not yet tackled and by-design limitations. User-visible limitations also
appear in the public-facing `docs/KNOWN_ISSUES.md`. Update this as items land.

Each item is tagged with its **nature**:
- **decision** — deliberately not done (cost/risk > value); no dependency, no trigger. "C3-spirit."
- **by-design** — there is a positive reason *not* to do it; effectively won't-fix.
- **follow-up** — a smaller loose end left by something we did land.
- **backlog** — a real conformance gap that just wasn't in a batch's scope; doable anytime.
- **acceptable-deviation** — divergence the review classified as fine for a rootless self-WM.

None of the currently-deferred items are *blocked/sequenced* on other work. (The
one item that genuinely waited on sequencing was E2 — it needed C9's
mapping-change infrastructure — and it is now done, v2.0.0.21.)

Last reviewed: 2026-09-16 (after R6 complete, v2.0.0.27-dbg).

---

## Deferred by decision (from the R0–R5 campaign)

- **C3 — sync-grab freeze/replay** — *decision*. `AllowEvents` is a documented
  no-op; `GrabModeSync` grabs never freeze. A faithful implementation (per-device
  frozen-event queue, freeze state machine, the full `AllowEvents`/`AllowSome`
  mode set, `ReplayPointer`/`ReplayKeyboard` re-injection + `PlayReleasedEvents`)
  is large and lands on the button/key input hot path Vivado uses daily, for
  near-zero value: Vivado (Swing) and Vitis (Electron/GTK) grab **async**, and
  the `SyncPointer` + `ReplayPointer` click-through scenario is the Motif/mwm
  *window-manager* idiom — but SwiftX11 is itself the rootless WM. A partial impl
  is worse than the no-op (a half-done ReplayPointer can hang a client). Revisit
  only if a real sync-grab client appears. Also listed in `KNOWN_ISSUES.md`.

- **B4 tail — full per-handler BadLength** — *decision (with one carve-out)*.
  `REQUEST_SIZE_MATCH` BadLength across ~100 request handlers, plus trailing-byte
  BadLength. Big mechanical surface, real regression risk, and no client depends
  on the general case (a short *core* request is caught by the reply-bearing
  safety net / BadImplementation today, which is benign). **Not** benign for
  reply-bearing *extension* requests: a truncated one whose `ByteReader` throws
  gets neither a reply nor an error, so that client hangs (review §2.4). The
  targeted fix is a ~20-line "any escaping extension major with no reply sent →
  BadImplementation" net — worth doing on its own; keep the rest deferred. The
  landed part of B4 — unknown-major → BadRequest, and len==0 / oversize →
  BadLength — is in v2.0.0.19.

- **RRSelectInput per-client tracking** — *by-design*. `RRScreenChangeNotify` is
  broadcast to all clients rather than tracked per subscriber. Over-delivery is
  the safe direction and is actively relied on for Xlib's `XRRUpdateConfiguration`
  popup-clip cache; stricter per-client delivery would only risk *under*-delivery.

- **GraphicsExpose for CopyArea** (§8 M4 tail) — *backlog*. On a CopyArea whose
  source is partly obscured/unavailable, the obscured regions should generate
  GraphicsExpose (NoExpose for a fully-available copy is already sent, F2). Low
  value here — our clients render via PutImage/backbuffers, not CopyArea from a
  visible-but-obscured source.

- **OpenFont BadName** (§8 / G12) — *backlog*. OpenFont on a missing font name
  falls back to a default instead of returning BadName; QueryFont on a bad fid
  returns the "fixed" fallback instead of BadFont. Benign for our clients.

## Follow-ups from what we built

- **G4 — top-level InputOnly window** — *follow-up*. A top-level (child-of-root)
  InputOnly window still gets an invisible NSWindow at map. GDK's InputOnly
  windows are children (common case covered); fully suppressing a top-level one
  needs an InputOnly flag on the create UI command.

- **G7 — embedder-death rescue is tree-integrity only** — *follow-up*. On rescue
  we reparent the survivor's window to root but do not emit ReparentNotify /
  MapNotify to the survivor's connection, and the rescued window has no NSWindow.
  Remapping does **not** recover it (an earlier note here wrongly said "until
  remapped"): there is no Reparent UI command, so the Swift-layer `WindowRegistry`
  parent tracking goes stale and a later map consults the wrong parent — the
  window stays invisible. The real fix is the Reparent/SaveSet Swift-layer
  integration (review §2.1; also in `KNOWN_ISSUES.md`), which covers
  cross-top-level `ReparentWindow` generally, not just the rescue.

- **GetImage depth/visual for non-24 pixmaps** (F5-era) — *follow-up*. GetImage on
  a pixmap reports depth-24, masking the alpha byte (surfaced during F5/F6).

## R3 rendering not done

- **F7–F10 rasterization deltas** — *backlog*. Rendering-precision items scoped
  out of R3 (F1–F6 landed and verified).
- **F4 optional extras** — *backlog*. RENDER clip-mask pixmaps + component-alpha.

## Review items never selected for a batch (lower severity, xorg-cited)

All *backlog* — in `docs/XORG_COMPARISON_REVIEW_2026-09-08.md` but not part of
R0–R5's targeted set:
- **B8** QueryTree truncates children at 256.
- **B9** GetWindowAttributes: no IsUnviewable map-state; `yourEventMask` returns
  the multi-client union rather than the caller's. (The *class* half was fixed in
  G4.)
- **B10** SetInputFocus: no revertTo>2 BadValue, no unviewable BadMatch, no
  timestamp gate.
- **B11** KillClient is a no-op; RetainTemporary ≡ RetainPermanent; no
  BadIDChoice/range checks on CreatePixmap/CreateGC/CreateCursor.
- **B12** assorted error-field nits (CreateWindow error precedence, BadMatch
  carrying a resource id, InternAtom truncation, GetAtomName(0), etc.).
- **G1** SendEvent: event-mask/propagate ignored, weak validation.
- **G8** DestroyWindow event order (no implicit UnmapNotify; parent-before-child).
- **G9** ConfigureWindow: win_gravity never applied on parent resize;
  CWSibling-without-CWStackMode BadMatch missing; no-op configures still emit
  ConfigureNotify+Expose.
- **G10** CirculateWindow: no occlusion test; RotateProperties no-op.
- **G13** same-connection ConvertSelection refusal (single-client PRIMARY
  self-paste).
- **G14** ConvertSelection MULTIPLE target unimplemented while acting as owner.
- **XC-MISC XID reclamation (midpoint allocator)** — freed XIDs are never
  returned to a client's range, so a resource-churning client burns its ~8M-XID
  allotment over a long session (within an order of magnitude of a week-long JVM
  that creates/destroys many pixmaps) and must reconnect to get a fresh range.
  `GetXIDRange` hands out a range; `GetXIDList` (reclamation) is unimplemented. A
  reclaiming allocator would lift the ceiling. Related to the RetainPermanent
  slot-recycle residual (R6.4).

## Acceptable rootless deviations (documented; not planned)

- **XI2**: L3 (valid as-is); L20 hierarchy/property events (static window tree);
  the **XI1 OpenDevice family** returns BadRequest by design (so `xinput
  query-state` / `get-button-map` don't work — XI2 and normal input do). Full
  audit: `docs/XI2_XORG_COMPARISON.md`.
- **Single 24-bit visual** — no depth-32 ARGB visual, so GTK RGBA / transparency
  is unavailable. A genuine roadmap enhancement (new visual + alpha compositing).
- **No WM-redirect events** (MapRequest/ConfigureRequest/CirculateRequest/
  ResizeRequest) and **no ShapeNotify** — acceptable since SwiftX11 is the WM.
- **AppKit "transaction during CA commit" warnings** on rapid create-then-destroy
  — harmless; predates the XI2 work.

## Done (kept for history)

- **Orphan `.hpp` cleanup** — ✅ done 2026-09-16 (the 7 dead `src/Extensions/`
  headers removed once the pbxproj was clean).
