// R5 G7 verification: SaveSet.  (A) ChangeSaveSet on your own window → BadMatch;
// (B) when an embedder client dies, a window it save-set (owned by a surviving
// client, reparented under the embedder's window) is rescued to root instead of
// being orphaned under the erased parent.
// Vivado-SAFE: nothing is mapped, so no windows appear; all destroyed at end.
//
// Build: cc -o r5_g7_test r5_g7_test.c -I/opt/X11/include -L/opt/X11/lib -lX11
// Run  : DISPLAY=127.0.0.1:1 ./r5_g7_test   (needs v2.0.0.25-dbg+)
#include <X11/Xlib.h>
#include <stdio.h>
#include <unistd.h>

static int lastErr = -1;
static int onErr(Display* d, XErrorEvent* e) { (void)d; lastErr = e->error_code; return 0; }

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  Display* dS = XOpenDisplay(NULL);   // the survivor client (owns WS)
  if (!dS) { fprintf(stderr, "cannot open display\n"); return 1; }
  XSetErrorHandler(onErr);
  Window root = DefaultRootWindow(dS);

  Window ws = XCreateSimpleWindow(dS, root, 0, 0, 30, 30, 0, 0, 0);
  XSync(dS, False);

  // (A) ChangeSaveSet on your OWN window → BadMatch.
  lastErr = -1;
  XAddToSaveSet(dS, ws);
  XSync(dS, False);
  printf("(A) ChangeSaveSet(own window): err=%d [expect BadMatch=%d]\n", lastErr, BadMatch);
  int passA = (lastErr == BadMatch);
  printf("   => %s\n", passA ? "PASS" : "FAIL");

  // (B) Embedder client: create WE, reparent WS under WE, save-set WS, then die.
  int passB = 0;
  Display* dE = XOpenDisplay(NULL);
  if (dE) {
    Window we = XCreateSimpleWindow(dE, root, 0, 0, 60, 60, 0, 0, 0);
    XSync(dE, False);
    XReparentWindow(dE, ws, we, 0, 0);   // embed the survivor's window under E's
    lastErr = -1;
    XAddToSaveSet(dE, ws);               // E save-sets another client's window → OK
    XSync(dE, False);
    int okInsert = (lastErr == -1);
    printf("(B0) ChangeSaveSet(other client's window): err=%d [expect none] => %s\n",
           lastErr, okInsert ? "ok" : "FAIL");

    XCloseDisplay(dE);                   // embedder dies

    // Survivor: WS should be rescued to root, not left under the erased WE.
    Window pp = 0, rr = 0, *kids = NULL; unsigned nk = 0;
    for (int i = 0; i < 30; i++) {
      usleep(50000);
      if (kids) { XFree(kids); kids = NULL; }
      if (XQueryTree(dS, ws, &rr, &pp, &kids, &nk) && pp == root) break;
    }
    if (kids) XFree(kids);
    printf("(B1) after embedder died: XQueryTree(WS) parent=0x%lx [expect root=0x%lx]\n",
           pp, root);
    passB = okInsert && (pp == root);
    printf("   => %s\n", passB ? "PASS" : "FAIL");
  } else {
    printf("(B) could not open a 2nd (embedder) connection\n");
  }

  XDestroyWindow(dS, ws);
  XSync(dS, False);
  XCloseDisplay(dS);
  int all = passA && passB;
  printf("=> ALL: %s\n", all ? "PASS" : "FAIL");
  return all ? 0 : 2;
}
