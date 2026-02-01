//
//  XProtoWindowCBridge.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/26/26.
//

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates/refreshes the C-side g_wins[] slot + g_framebuffers[] for wid.
// Mirrors the same behavior as the old handle_CreateWindow, but WITHOUT parsing.
// Returns 1 on success, 0 on failure.
int x11_xproto_c_create_window_slot(uint32_t wid,
                                   uint32_t parent,
                                   int16_t x,
                                   int16_t y,
                                   uint16_t wpx,
                                   uint16_t hpx,
                                   uint32_t event_mask,
                                   int owner_fd,
                                   int* out_dirty /* optional, may be NULL */);

void x11_backend_fb_destroy(uint32_t wid);

#ifdef __cplusplus
}
#endif
