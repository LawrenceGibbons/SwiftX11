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
