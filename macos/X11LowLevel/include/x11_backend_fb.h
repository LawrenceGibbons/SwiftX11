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

int  x11_xproto_window_fb_rw(uint32_t xid, uint32_t** outPixels, uint32_t* outW, uint32_t* outH);

int  x11_backend_fb_create_slot(uint32_t wid, uint16_t wpx, uint16_t hpx, int owner_fd, int* out_dirty);
void x11_backend_fb_resize(uint32_t wid, uint16_t new_w, uint16_t new_h);
void x11_backend_fb_destroy(uint32_t wid);

#ifdef __cplusplus
}
#endif
