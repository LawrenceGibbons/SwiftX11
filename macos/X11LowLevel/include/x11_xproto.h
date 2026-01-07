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

#ifdef __cplusplus
}
#endif
