//
//  XProtoWindowBridge.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Applies the side-effects of MapWindow to C truth:
// - sets w->mapped = 1
// - enqueues shim map request
// - if w->dirty && w->presentable: clears dirty and enqueues damage
//
// Returns 1 if the window exists and was mapped, 0 otherwise.
int x11_xproto_apply_map_window(uint32_t wid);

// Sets g_wins[xid].mapped=0 and returns 1 if window existed, else 0.
int x11_xproto_window_set_mapped(uint32_t wid, int mapped);

  

#ifdef __cplusplus
}
#endif
