// r6_test.c — verify R6.2, R6.3, R6.5, R6.6 on v2.0.0.27-dbg.
//
// Mostly Vivado-safe: R6.3 / R6.5 / R6.6 map nothing (unmapped windows +
// pixmaps + private atoms — the real clipboard is untouched).  R6.2 briefly
// maps a tiny override-redirect window in the top-left corner (no focus steal)
// and destroys it — a ~0.5 s flash.
//
// Build: cc -o r6_test r6_test.c -I/opt/X11/include -L/opt/X11/lib -lX11
// Run:   DISPLAY=127.0.0.1:1 ./r6_test
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <unistd.h>

static int g_err = 0;
static int onErr(Display* d, XErrorEvent* e) { (void)d; g_err = e->error_code; return 0; }

// Poll up to `ms` for an event of `type`; return 1 if seen.
static int wait_for_type(Display* d, int type, XEvent* out, int ms) {
  for (int i = 0; i < ms / 5; i++) {
    while (XPending(d)) { XNextEvent(d, out); if (out->type == type) return 1; }
    XSync(d, False);
    usleep(5000);
  }
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  Display* d = XOpenDisplay(NULL);
  if (!d) { fprintf(stderr, "cannot open display\n"); return 1; }
  XSetErrorHandler(onErr);
  Window root = DefaultRootWindow(d);
  int pass = 0, total = 0;

  // ---- R6.3: DestroySubwindows sweeps selection owners ----
  total++;
  {
    Atom sel = XInternAtom(d, "SWIFTX11_R6_TEST_SEL", False);   // private, NOT CLIPBOARD
    Window parent = XCreateSimpleWindow(d, root, 0, 0, 50, 50, 0, 0, 0);
    Window child  = XCreateSimpleWindow(d, parent, 0, 0, 20, 20, 0, 0, 0);
    XSync(d, False);
    XSetSelectionOwner(d, sel, child, CurrentTime);
    XSync(d, False);
    Window before = XGetSelectionOwner(d, sel);
    XDestroySubwindows(d, parent);                              // destroys child
    XSync(d, False);
    Window after = XGetSelectionOwner(d, sel);
    int ok = (before == child) && (after == None);
    printf("[R6.3] DestroySubwindows selection sweep: before=0x%lx after=0x%lx => %s\n",
           before, after, ok ? "PASS" : "FAIL");
    pass += ok;
    XDestroyWindow(d, parent);
    XSync(d, False);
  }

  // ---- R6.6: NoExpose on a degenerate (clamped-out) CopyArea ----
  total++;
  {
    Pixmap sp = XCreatePixmap(d, root, 20, 20, 24);
    Pixmap dp = XCreatePixmap(d, root, 20, 20, 24);
    XGCValues gv; gv.graphics_exposures = True;
    GC gc = XCreateGC(d, dp, GCGraphicsExposures, &gv);
    XSync(d, False);
    // Source rect entirely outside the 20x20 pixmap => clamped to nothing =>
    // an early return that must still emit NoExpose, or a graphics_exposures
    // client blocks forever.
    XCopyArea(d, sp, dp, gc, 50, 50, 10, 10, 0, 0);
    XSync(d, False);
    XEvent ev;
    int got = wait_for_type(d, NoExpose, &ev, 1500);
    printf("[R6.6] NoExpose on clamped-out CopyArea: got=%d => %s\n",
           got, got ? "PASS" : "FAIL");
    pass += got;
    XFreeGC(d, gc); XFreePixmap(d, sp); XFreePixmap(d, dp);
    XSync(d, False);
  }

  // ---- R6.2: GetImage on a child clipped by its host returns the full size ----
  total++;
  {
    XSetWindowAttributes wa;
    wa.override_redirect = True;
    wa.background_pixel   = 0x00204060;
    Window host = XCreateWindow(d, root, 0, 0, 60, 60, 0, CopyFromParent, InputOutput,
                                CopyFromParent, CWOverrideRedirect | CWBackPixel, &wa);
    // Child at (40,40) size 40x40 spills past the 60x60 host => its materialised
    // surface extent is 20x20 < its 40x40 geometry.  Pre-fix this BadMatch'd.
    Window child = XCreateSimpleWindow(d, host, 40, 40, 40, 40, 0, 0, 0x00806040);
    XMapWindow(d, host);
    XMapWindow(d, child);
    XSync(d, False);
    usleep(600000);                                            // let the host surface materialise
    g_err = 0;
    XImage* img = XGetImage(d, child, 0, 0, 40, 40, AllPlanes, ZPixmap);
    XSync(d, False);
    int ok = (img != NULL) && (g_err == 0) && img->width == 40 && img->height == 40;
    printf("[R6.2] XGetImage(clipped child, 0,0,40,40): img=%p err=%d %dx%d => %s\n",
           (void*)img, g_err, img ? img->width : 0, img ? img->height : 0,
           ok ? "PASS" : "FAIL");
    pass += ok;
    if (img) XDestroyImage(img);
    XDestroyWindow(d, host);                                   // destroys child too
    XSync(d, False);
  }

  // ---- R6.5: a foreign child of a dying client is destroyed (DestroyNotify) ----
  total++;
  {
    Display* dB = XOpenDisplay(NULL);   // survivor
    Display* dA = XOpenDisplay(NULL);   // embedder (will die)
    if (dB && dA) {
      Window wA = XCreateSimpleWindow(dA, DefaultRootWindow(dA), 0, 0, 50, 50, 0, 0, 0);
      XSync(dA, False);
      Window wB = XCreateSimpleWindow(dB, DefaultRootWindow(dB), 0, 0, 30, 30, 0, 0, 0);
      XSelectInput(dB, wB, StructureNotifyMask);
      XSync(dB, False);
      XReparentWindow(dB, wB, wA, 0, 0);                       // cross-client: wB under wA
      XSync(dB, False);
      { XEvent e; while (XPending(dB)) XNextEvent(dB, &e); }   // drain reparent notifies
      XCloseDisplay(dA);                                       // embedder dies
      XEvent ev; int got = 0;
      for (int i = 0; i < 300 && !got; i++) {
        while (XPending(dB)) {
          XNextEvent(dB, &ev);
          if (ev.type == DestroyNotify && ev.xdestroywindow.window == wB) got = 1;
        }
        XSync(dB, False); usleep(5000);
      }
      printf("[R6.5] foreign child destroyed on embedder death: DestroyNotify=%d => %s\n",
             got, got ? "PASS" : "FAIL");
      pass += got;
      XCloseDisplay(dB);
    } else {
      printf("[R6.5] SKIP (could not open extra connections)\n");
      total--;
      if (dB) XCloseDisplay(dB);
      if (dA) XCloseDisplay(dA);
    }
  }

  printf("=== R6 probe: %d/%d PASS ===\n", pass, total);
  XCloseDisplay(d);
  return (pass == total) ? 0 : 2;
}
