//
//  x11_backend_fb.h
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/4/26.
//

#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*x11_fb_event_fn)(
  uint32_t xid,
  uint16_t w,
  uint16_t h,
  const char* op,   // "create" | "resize"
  const char* why,  // "CreateWindow", "ConfigureWindow", ...
  const char* file,
  int line,
  void* user);

  
  // new for debugging
  void x11_backend_fb_set_event_observer(x11_fb_event_fn fn, void* user);

  // Debug-friendly wrappers that carry why/file/line (call these from C++)
  void x11_backend_fb_resize_dbg(uint32_t wid, uint16_t new_w, uint16_t new_h,
                                 const char* why, const char* file, int line);

  int  x11_backend_fb_create_slot_dbg(uint32_t wid, uint16_t wpx, uint16_t hpx,
                                     int owner_fd, int* out_dirty,
                                     const char* why, const char* file, int line);

  // -- the older versions
// Returns 1 on success, 0 on failure.
// outPixels is a pointer to the window framebuffer (ARGB/BGRA as you currently store it),
// outW/outH are pixel dimensions.
int x11_xproto_window_fb_rw(uint32_t xid,
                              uint32_t** outPixels,
                              uint32_t* outW,
                              uint32_t* outH);

void x11_backend_fb_resize_impl(uint32_t wid, uint16_t new_w, uint16_t new_h);
  
// Creates/refreshes the C-side g_fb[] slot for wid.
int x11_backend_fb_create_slot(uint32_t wid,
                               uint16_t wpx,
                               uint16_t hpx,
                               int owner_fd,
                               int* out_dirty /* optional, may be NULL */);

void x11_backend_fb_destroy(uint32_t wid);

#ifdef __cplusplus
}
#endif
