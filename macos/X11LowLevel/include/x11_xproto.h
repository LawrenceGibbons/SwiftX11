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

// Start/stop the X11 TCP listener on :display (port 6000+display).
void x11_xproto_listener_start(int display);
void x11_xproto_listener_stop(void);

// server-thread only:
void x11_xproto_set_window_presentable(uint32_t xid);
  
// x11_xproto_fb.h
int x11_xproto_copy_window_bgra(uint32_t xid,
                                uint8_t* out_bytes,
                                int32_t out_cap,
                                int32_t* out_w,
                                int32_t* out_h,
                                int32_t* out_bpr);
  
#ifdef __cplusplus
}
#endif
