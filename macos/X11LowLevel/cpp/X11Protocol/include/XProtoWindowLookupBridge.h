//
//  XProtoWindowLookupBridge.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/23/26.
//

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fill out window view fields for a given xid from the C xproto state.
// Returns 1 if found, 0 if not found.
int x11_xproto_snapshot_window_view(uint32_t xid,
                                   int16_t* out_x,
                                   int16_t* out_y,
                                   uint16_t* out_w,
                                   uint16_t* out_h,
                                   uint32_t* out_event_mask,
                                   int* out_mapped,
                                   int* out_owner_fd);

#ifdef __cplusplus
}
#endif
