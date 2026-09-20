// R6.1 verification — DestroyWindow(root) and UnmapWindow(root) must be no-ops.
//
// DANGER: run this ONLY against a build that HAS the R6.1 guard (v2.0.0.26+).
// Against an unguarded build it will DESTROY every window of every client
// (including Vivado/Vitis). On a fixed build both requests are silent no-ops.
//
// Build: cc -o r6_1_test r6_1_test.c -I/opt/X11/include -L/opt/X11/lib -lX11
// Run:   DISPLAY=127.0.0.1:1 ./r6_1_test
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>

static int lastErr = -1;
static int onErr(Display *d, XErrorEvent *e) { (void)d; lastErr = e->error_code; return 0; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "cannot open display\n"); return 1; }
    XSetErrorHandler(onErr);
    Window root = DefaultRootWindow(d);

    // A marker window whose survival proves the server was not torn down.
    Window marker = XCreateSimpleWindow(d, root, 0, 0, 20, 20, 0, 0, 0);
    XSync(d, False);

    Atom netSupported = XInternAtom(d, "_NET_SUPPORTED", True);

    // (1) DestroyWindow(root) — expect a silent no-op.
    lastErr = -1;
    XDestroyWindow(d, root);
    XSync(d, False);
    int destroyErr = lastErr;

    // (2) UnmapWindow(root) — expect a silent no-op; root stays Viewable.
    lastErr = -1;
    XUnmapWindow(d, root);
    XSync(d, False);
    int unmapErr = lastErr;

    // Survivors.
    lastErr = -1;
    XWindowAttributes wa;
    int markerAlive = (XGetWindowAttributes(d, marker, &wa) && lastErr == -1);

    XWindowAttributes ra;
    XGetWindowAttributes(d, root, &ra);
    int rootViewable = (ra.map_state == IsViewable);

    int propOk = 0;
    if (netSupported) {
        Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
        XGetWindowProperty(d, root, netSupported, 0, 64, False, AnyPropertyType,
                           &type, &fmt, &n, &after, &data);
        propOk = (type != None && n > 0);
        if (data) XFree(data);
    }

    printf("DestroyWindow(root): err=%d  [expect -1, no error]\n", destroyErr);
    printf("UnmapWindow(root):   err=%d  [expect -1, no error]\n", unmapErr);
    printf("marker alive=%d | root Viewable=%d | _NET_SUPPORTED present=%d\n",
           markerAlive, rootViewable, propOk);
    int pass = (destroyErr == -1) && (unmapErr == -1) && markerAlive && rootViewable && propOk;
    printf("=> %s\n", pass ? "PASS" : "FAIL");

    XDestroyWindow(d, marker);
    XSync(d, False);
    XCloseDisplay(d);
    return pass ? 0 : 2;
}
