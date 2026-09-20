// R5 G4 verification: InputOnly window class + cross-client parents.
// Vivado-SAFE: nothing is mapped, so no windows appear on screen; all created
// windows are destroyed at the end.
//
// Build: cc -o r5_g4_test r5_g4_test.c -I/opt/X11/include -L/opt/X11/lib -lX11
// Run  : DISPLAY=127.0.0.1:1 ./r5_g4_test   (needs v2.0.0.23-dbg+)
#include <X11/Xlib.h>
#include <stdio.h>

static int lastErr = -1;
static int onErr(Display* d, XErrorEvent* e) { (void)d; lastErr = e->error_code; return 0; }

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  Display* d1 = XOpenDisplay(NULL);
  if (!d1) { fprintf(stderr, "cannot open display\n"); return 1; }
  XSetErrorHandler(onErr);   // Xlib error handler is process-global (covers d2 too)
  Window root = DefaultRootWindow(d1);

  XSetWindowAttributes sa;
  sa.event_mask = PointerMotionMask;

  // (1) InputOnly window: created OK, class reported as InputOnly, visual None.
  Window io = XCreateWindow(d1, root, 0, 0, 30, 30, 0 /*border*/, 0 /*depth*/,
                            InputOnly, CopyFromParent, CWEventMask, &sa);
  XSync(d1, False);
  XWindowAttributes wa;
  Status st = XGetWindowAttributes(d1, io, &wa);
  printf("(1) InputOnly: getattrs ok=%d class=%d [InputOnly=%d]\n", (int)st, wa.class, InputOnly);
  int pass1 = (st && wa.class == InputOnly);
  printf("   => %s\n", pass1 ? "PASS" : "FAIL");

  // (1b) InputOnly with a REAL visual (as GDK3 passes) must succeed — an
  // InputOnly window may specify a visual (v2.0.0.24 regression guard).
  lastErr = -1;
  Visual* vis = DefaultVisual(d1, DefaultScreen(d1));
  Window ioVis = XCreateWindow(d1, root, 0, 0, 20, 20, 0, 0, InputOnly, vis, CWEventMask, &sa);
  XSync(d1, False);
  printf("(1b) InputOnly + real visual: err=%d [expect none]\n", lastErr);
  int pass1b = (lastErr == -1);
  printf("   => %s\n", pass1b ? "PASS" : "FAIL");
  if (ioVis) XDestroyWindow(d1, ioVis);

  // (2) InputOnly with a forbidden pixel attribute (CWBackPixel) → BadMatch.
  lastErr = -1;
  sa.background_pixel = 0xff0000;
  (void)XCreateWindow(d1, root, 0, 0, 30, 30, 0, 0, InputOnly, CopyFromParent,
                      CWEventMask | CWBackPixel, &sa);
  XSync(d1, False);
  printf("(2) InputOnly + CWBackPixel: err=%d [expect BadMatch=%d]\n", lastErr, BadMatch);
  int pass2 = (lastErr == BadMatch);
  printf("   => %s\n", pass2 ? "PASS" : "FAIL");

  // (3) An explicit InputOutput child of an InputOnly parent → BadMatch.
  Window ioParent = XCreateWindow(d1, root, 0, 0, 30, 30, 0, 0, InputOnly,
                                  CopyFromParent, CWEventMask, &sa);
  XSync(d1, False);
  lastErr = -1;
  (void)XCreateWindow(d1, ioParent, 0, 0, 10, 10, 0, 0 /*depth*/, InputOutput,
                      CopyFromParent, 0, NULL);
  XSync(d1, False);
  printf("(3) InputOutput under InputOnly: err=%d [expect BadMatch=%d]\n", lastErr, BadMatch);
  int pass3 = (lastErr == BadMatch);
  printf("   => %s\n", pass3 ? "PASS" : "FAIL");

  // (4) Cross-client parent: a SECOND client creates a window under d1's window.
  Window a = XCreateSimpleWindow(d1, root, 0, 0, 40, 40, 0, 0, 0);
  XSync(d1, False);
  int pass4 = 0;
  Display* d2 = XOpenDisplay(NULL);
  if (d2) {
    lastErr = -1;
    Window b = XCreateSimpleWindow(d2, a /*parent owned by client d1*/, 0, 0, 10, 10, 0, 0, 0);
    XSync(d2, False);
    printf("(4) client-2 CreateWindow(parent=client-1's window): err=%d [expect none]\n", lastErr);
    pass4 = (lastErr == -1);
    if (b) { XDestroyWindow(d2, b); XSync(d2, False); }
    XCloseDisplay(d2);
  } else {
    printf("(4) could not open a 2nd display connection\n");
  }
  printf("   => %s\n", pass4 ? "PASS" : "FAIL");

  XDestroyWindow(d1, io);
  XDestroyWindow(d1, ioParent);
  XDestroyWindow(d1, a);
  XSync(d1, False);
  XCloseDisplay(d1);
  int all = pass1 && pass1b && pass2 && pass3 && pass4;
  printf("=> ALL: %s\n", all ? "PASS" : "FAIL");
  return all ? 0 : 2;
}
