//
//  x11_setup.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/3/26.
//

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void x11_send_setup_failed_le(int fd, const char* msg);
void x11_send_setup_success_minimal_little_endian(int fd);

#ifdef __cplusplus
}
#endif

