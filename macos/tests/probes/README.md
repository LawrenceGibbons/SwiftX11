# SwiftX11 regression probes

Standalone X11 client programs that assert correct-vs-wrong server behavior
against a **running** SwiftX11 (display `:1`). Each returns exit code `0` on
PASS, non-zero on FAIL, and prints a per-check line. They mirror what real
clients (GDK, Java/AWT, libX11) actually send — a happy-path probe once passed
while a real client broke (the G4 InputOnly visual case), so probes here use
real-client patterns, not just the spec's easy path.

These cover the R5/R6 batches of the 2026-09-08 / 2026-09-16 reviews. The
earlier R0–R4 probes (`r1_test`, `r1_p2/p3/p4_test`, `r2_test`, `r4_test`,
`clip_test`) were written in an ephemeral scratchpad and lost before they could
be preserved; their pass records and the exact behavior they checked are in
`docs/CLAUDE.md` (search the version tags), so they can be re-created if an
R0–R4 path ever regresses.

## Build & run

With SwiftX11 running (banner should read the version under test):

```bash
cc -o r6_test r6_test.c -I/opt/X11/include -L/opt/X11/lib -lX11
DISPLAY=127.0.0.1:1 ./r6_test        # exit 0 = PASS
```

Some probes also need `-lXi` (XInput2) or `-lXtst` (XTEST) — noted below.

## Probes

| File | Verifies | Safety |
|---|---|---|
| `r5_e2_test.c` | XKB keymap rebuilds on a core mapping change + `XkbMapNotify` (v2.0.0.21 / E2) | Vivado-safe — remaps a spare NoSymbol keycode, then restores it |
| `r5_g6_test.c` | `ReparentWindow` choreography (UnmapNotify → ReparentNotify → MapNotify) + BadWindow on a bogus new parent (v2.0.0.22 / G6) | Vivado-safe — maps nothing |
| `r5_g4_test.c` | `InputOnly` window class + cross-client parents; includes the real-visual regression guard (v2.0.0.23/.24 / G4) | Vivado-safe — 2nd connection, maps nothing |
| `g4_repro.c` | Minimal repro: `XCreateWindow(InputOnly, real visual)` must succeed (the .24 hotfix that unbroke Vitis) | Vivado-safe |
| `r5_g7_test.c` | `ChangeSaveSet` validation + embedder-death rescue reparents the survivor's window to root (v2.0.0.25 / G7) | Vivado-safe — two connections, maps nothing |
| `r6_test.c` | R6.2 GetImage on a host-clipped child returns full size (no BadMatch); R6.3 DestroySubwindows sweeps selection owners; R6.5 foreign child gets DestroyNotify on embedder death; R6.6 NoExpose on a clamped-out CopyArea | R6.3/R6.5/R6.6 map nothing; **R6.2 briefly maps a tiny 60×60 override-redirect window** (top-left corner, ~0.6 s) then destroys it |
| `r6_1_test.c` | R6.1 `DestroyWindow(root)` / `UnmapWindow(root)` are silent no-ops and the server survives (v2.0.0.26) | ⚠️ **DESTRUCTIVE on an unguarded build** — calls `DestroyWindow(root)`, which is a no-op only on **v2.0.0.26+**. On any earlier build it destroys every window of every client. Never run it against a build without the R6.1 guard. |

## Notes

- "Vivado-safe" means the probe maps no on-screen windows and restores any state
  it touches, so it can run while Vivado/Vitis are up without disturbing them —
  except `r6_test`'s R6.2 sub-test, which flashes a tiny corner window.
- The probes assume the advertised visual/depth of v2.0.0 (24-bit TrueColor,
  root XID `0x2`). Update them if those change.
