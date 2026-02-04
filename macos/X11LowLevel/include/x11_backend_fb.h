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

// Returns 1 on success, 0 on failure.
// outPixels is a pointer to the window framebuffer (ARGB/BGRA as you currently store it),
// outW/outH are pixel dimensions.
int x11_xproto_window_fb_rw(uint32_t xid,
                              uint32_t** outPixels,
                              uint32_t* outW,
                              uint32_t* outH);

void x11_backend_fb_resize(uint32_t wid, uint16_t new_w, uint16_t new_h);

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
