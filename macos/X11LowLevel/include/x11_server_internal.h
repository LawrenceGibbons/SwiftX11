//
//  x11_server_internal.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/6/26.
//

#ifndef x11_server_internal_h
#define x11_server_internal_h

// x11_server_internal.h (not installed, project-private)
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void x11_server_emit_window_create(uint32_t xid, uint32_t parent_xid, const char* title, int32_t w_px, int32_t h_px);
void x11_server_emit_window_destroy(uint32_t xid);

void x11_server_apply_map_request(uint32_t xid);
void x11_server_apply_unmap_request(uint32_t xid);
void x11_server_apply_configure_request(uint32_t xid, int32_t w_u, int32_t h_u);

uint64_t x11_now_ns(void); // if needed

#ifdef __cplusplus
}
#endif

#endif /* x11_server_internal_h */
