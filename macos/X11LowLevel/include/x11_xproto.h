//
//  x11_xproto.h
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/7/26.
//

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Copy a window's current backing-store pixels into BGRA bytes for Swift/UI.
int x11_xproto_copy_window_bgra(uint32_t xid,
                                uint8_t* out_bytes,
                                int32_t out_cap,
                                int32_t* out_w,
                                int32_t* out_h,
                                int32_t* out_bpr);

#ifdef __cplusplus
}
#endif
