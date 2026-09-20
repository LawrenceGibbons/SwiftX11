// R5 G6 verification: ReparentWindow of a mapped window emits UnmapNotify +
// MapNotify (to the window's StructureNotify and the old/new parent's
// SubstructureNotify selectors), and a bogus new parent returns BadWindow.
// Vivado-SAFE: the two parents are created but never mapped, so no windows
// appear on screen; the child is mapped-but-not-viewable, which still drives
// the auto-unmap/remap. Everything is destroyed at the end.
//
// Build: cc -o r5_g6_test r5_g6_test.c -I/opt/X11/include -L/opt/X11/lib -lX11
// Run  : DISPLAY=127.0.0.1:1 ./r5_g6_test   (needs v2.0.0.22-dbg+)
#include <X11/Xlib.h>
#include <stdio.h>
#include <unistd.h>

static int lastErr = -1;
static int onErr(Display* d, XErrorEvent* e) { (void)d; lastErr = e->error_code; return 0; }

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  Display* dpy = XOpenDisplay(NULL);
  if (!dpy) { fprintf(stderr, "cannot open display\n"); return 1; }
  XSetErrorHandler(onErr);
  Window root = DefaultRootWindow(dpy);

  // Two toplevel parents (created, NOT mapped → nothing shown), each watching
  // SubstructureNotify; a child W under P1 watching StructureNotify, mapped.
  Window p1 = XCreateSimpleWindow(dpy, root, 0, 0, 40, 40, 0, 0, 0);
  Window p2 = XCreateSimpleWindow(dpy, root, 0, 0, 40, 40, 0, 0, 0);
  Window w  = XCreateSimpleWindow(dpy, p1,   0, 0, 20, 20, 0, 0, 0);
  XSelectInput(dpy, p1, SubstructureNotifyMask);
  XSelectInput(dpy, p2, SubstructureNotifyMask);
  XSelectInput(dpy, w,  StructureNotifyMask);
  XMapWindow(dpy, w);            // mapped, but P1 is unmapped → not viewable
  XSync(dpy, False);
  // Drain the map-time events so we only see the reparent's below.
  XEvent tmp; while (XPending(dpy)) XNextEvent(dpy, &tmp);

  printf("p1=0x%lx p2=0x%lx w=0x%lx\n", p1, p2, w);

  // Reparent W from P1 to P2.
  XReparentWindow(dpy, w, p2, 0, 0);
  XSync(dpy, False);

  int unmapW = 0, unmapP1 = 0, reparent = 0, mapW = 0, mapP2 = 0;
  for (int i = 0; i < 50; i++) {
    while (XPending(dpy)) {
      XEvent e; XNextEvent(dpy, &e);
      switch (e.type) {
        case UnmapNotify:
          if (e.xunmap.window == w && e.xany.window == w)  unmapW  = 1;
          if (e.xunmap.window == w && e.xany.window == p1) unmapP1 = 1;
          break;
        case ReparentNotify:
          if (e.xreparent.window == w && e.xreparent.parent == p2) reparent = 1;
          break;
        case MapNotify:
          if (e.xmap.window == w && e.xany.window == w)  mapW  = 1;
          if (e.xmap.window == w && e.xany.window == p2) mapP2 = 1;
          break;
      }
    }
    if (unmapW && unmapP1 && reparent && mapW && mapP2) break;
    usleep(20000);
  }
  printf("UnmapNotify: onW=%d onP1=%d | ReparentNotify: %d | MapNotify: onW=%d onP2=%d\n",
         unmapW, unmapP1, reparent, mapW, mapP2);
  int passEvents = unmapW && unmapP1 && reparent && mapW && mapP2;
  printf("=> (events) %s\n", passEvents ? "PASS" : "FAIL");

  // Bogus new parent → BadWindow.
  lastErr = -1;
  Window bogus = 0x03fffffe;   // not a real window
  XReparentWindow(dpy, w, bogus, 0, 0);
  XSync(dpy, False);
  printf("reparent to bogus 0x%lx: err=%d  [expect BadWindow=%d]\n",
         bogus, lastErr, BadWindow);
  int passBadWin = (lastErr == BadWindow);
  printf("=> (BadWindow) %s\n", passBadWin ? "PASS" : "FAIL");

  XDestroyWindow(dpy, w);
  XDestroyWindow(dpy, p1);
  XDestroyWindow(dpy, p2);
  XSync(dpy, False);
  XCloseDisplay(dpy);
  return (passEvents && passBadWin) ? 0 : 2;
}
