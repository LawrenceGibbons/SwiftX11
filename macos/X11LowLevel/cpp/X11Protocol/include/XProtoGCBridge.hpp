//
//  XProtoServerBridge.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/28/26.
//

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 if found, 0 if missing.
// If missing, out_fg/out_bg are left unchanged (caller should default).
int x11_proto_bridge_gc_get(uint32_t gc_xid, uint32_t* out_fg, uint32_t* out_bg);

#ifdef __cplusplus
} // extern "C"
#endif
