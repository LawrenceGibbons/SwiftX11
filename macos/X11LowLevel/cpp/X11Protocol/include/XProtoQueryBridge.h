//
//  XProtoQueryBridge.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 on success, 0 on failure.
// out_children may be NULL if max_children==0.
// nchildren is written to *out_nchildren (0 if none).
int x11_xproto_query_tree(uint32_t wid,
                          uint32_t* out_parent,
                          uint32_t* out_children,
                          uint32_t  max_children,
                          uint32_t* out_nchildren);

#ifdef __cplusplus
}
#endif
