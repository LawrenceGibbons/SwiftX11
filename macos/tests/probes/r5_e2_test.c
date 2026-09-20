// R5 Phase 1 (E2) verification: the XKB map rebuilds on a core mapping change
// and XkbMapNotify is delivered.  Vivado-SAFE: it remaps only a *spare* keycode
// (NoSymbol in every column, so no client uses it) and restores it afterward.
//
// Build: cc -o r5_e2_test r5_e2_test.c -I/opt/X11/include -L/opt/X11/lib -lX11
// Run  : DISPLAY=127.0.0.1:1 ./r5_e2_test   (needs v2.0.0.21-dbg+)
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  Display* dpy = XOpenDisplay(NULL);
  if (!dpy) { fprintf(stderr, "cannot open display\n"); return 1; }

  int maj = XkbMajorVersion, min = XkbMinorVersion, xkbEvBase = 0, xkbErrBase = 0;
  if (!XkbQueryExtension(dpy, NULL, &xkbEvBase, &xkbErrBase, &maj, &min)) {
    printf("XKB not present — FAIL\n"); XCloseDisplay(dpy); return 1;
  }
  printf("XKB present: event_base=%d version=%d.%d\n", xkbEvBase, maj, min);

  int minKc = 0, maxKc = 0;
  XDisplayKeycodes(dpy, &minKc, &maxKc);
  int kpc = 0;
  KeySym* full = XGetKeyboardMapping(dpy, minKc, maxKc - minKc + 1, &kpc);
  int spare = 0;
  for (int kc = maxKc; kc >= minKc && !spare; kc--) {
    int idx = (kc - minKc) * kpc, allzero = 1;
    for (int j = 0; j < kpc; j++) if (full[idx + j] != NoSymbol) { allzero = 0; break; }
    if (allzero) spare = kc;
  }
  if (full) XFree(full);
  printf("keycodes %d..%d, kpc=%d, spare=%d\n", minKc, maxKc, kpc, spare);
  if (!spare) { printf("no spare keycode — aborting to avoid disturbing Vivado\n"); XCloseDisplay(dpy); return 0; }

  // Select XkbMapNotify for all map components.
  XkbSelectEvents(dpy, XkbUseCoreKbd, XkbMapNotifyMask, XkbMapNotifyMask);
  XkbSelectEventDetails(dpy, XkbUseCoreKbd, XkbMapNotify,
                        XkbAllMapComponentsMask, XkbAllMapComponentsMask);
  XSync(dpy, False);

  // Remap the spare keycode to a distinctive keysym.
  const KeySym NEWSYM = XK_F35;
  KeySym one[1] = { NEWSYM };
  XChangeKeyboardMapping(dpy, spare, 1, one, 1);
  XSync(dpy, False);

  // (a) XkbMapNotify delivered?  Drain briefly.
  int gotMapNotify = 0; unsigned changed = 0;
  for (int i = 0; i < 50 && !gotMapNotify; i++) {
    while (XPending(dpy)) {
      XkbEvent ev; XNextEvent(dpy, &ev.core);
      if (ev.type == xkbEvBase && ev.any.xkb_type == XkbMapNotify) {
        gotMapNotify = 1; changed = ev.map.changed;
      }
    }
    if (!gotMapNotify) usleep(20000);
  }
  printf("(a) XkbMapNotify: %s  changed=0x%x  [expect delivered, KeySyms bit 0x2 set]\n",
         gotMapNotify ? "delivered" : "NOT delivered", changed);

  // (b) Fresh XkbGetMap reflects the new keysym (proves the server rebuilt).
  KeySym got = NoSymbol;
  XkbDescPtr xkb = XkbGetMap(dpy, XkbKeySymsMask, XkbUseCoreKbd);
  if (xkb && xkb->map && xkb->map->key_sym_map && XkbKeyNumSyms(xkb, spare) > 0)
    got = xkb->map->syms[xkb->map->key_sym_map[spare].offset];
  printf("(b) XkbGetMap[keycode %d] group0/level0 = 0x%lx  [expect XK_F35=0x%lx]\n",
         spare, (unsigned long)got, (unsigned long)NEWSYM);
  if (xkb) XkbFreeKeyboard(xkb, XkbAllComponentsMask, True);

  // (c) Core GetKeyboardMapping reflects it too (C9 sanity).
  KeySym coreGot = NoSymbol;
  KeySym* m = XGetKeyboardMapping(dpy, spare, 1, &kpc);
  if (m) { coreGot = m[0]; XFree(m); }
  printf("(c) core GetKeyboardMapping[%d][0] = 0x%lx  [expect XK_F35]\n",
         spare, (unsigned long)coreGot);

  int passA = gotMapNotify && (changed & XkbKeySymsMask);
  int passB = (got == NEWSYM);
  int passC = (coreGot == NEWSYM);
  printf("=> (a) MapNotify %s | (b) XKB rebuilt %s | (c) core %s\n",
         passA ? "PASS" : "FAIL", passB ? "PASS" : "FAIL", passC ? "PASS" : "FAIL");

  // Restore the spare keycode so the server keymap is left as found.
  KeySym none[1] = { NoSymbol };
  XChangeKeyboardMapping(dpy, spare, 1, none, 1);
  XSync(dpy, False);
  printf("restored keycode %d to NoSymbol\n", spare);

  XCloseDisplay(dpy);
  return (passA && passB && passC) ? 0 : 2;
}
