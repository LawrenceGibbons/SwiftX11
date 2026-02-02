//
//  X11BackendFBBridge.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/1/26.
//

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

  // Returns 1 on success, 0 on failure.
  // outPixels points at the window framebuffer (32bpp), outW/outH are pixel dims.
  int x11_xproto_window_fb_rw(uint32_t xid,
                              uint32_t** outPixels,
                              uint32_t* outW,
                              uint32_t* outH);
  
  
#ifdef __cplusplus
}
#endif
