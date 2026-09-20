#include <X11/Xlib.h>
#include <stdio.h>
static int lastErr=-1; static int onErr(Display*d,XErrorEvent*e){(void)d;lastErr=e->error_code;return 0;}
int main(void){
  Display* d=XOpenDisplay(NULL); if(!d){fprintf(stderr,"no display\n");return 1;}
  XSetErrorHandler(onErr);
  Window root=DefaultRootWindow(d);
  Visual* vis=DefaultVisual(d,DefaultScreen(d));   // a REAL visual, as GDK passes
  XSetWindowAttributes sa; sa.event_mask=PointerMotionMask;
  lastErr=-1;
  Window io=XCreateWindow(d,root,0,0,20,20,0,0,InputOnly,vis,CWEventMask,&sa);
  XSync(d,False);
  printf("InputOnly + REAL visual: err=%d  [BadMatch=%d]  %s\n",
         lastErr, BadMatch, lastErr==BadMatch ? "<-- REPRODUCED the regression" : "(ok)");
  if(io) XDestroyWindow(d,io);
  XSync(d,False); XCloseDisplay(d); return 0;
}
