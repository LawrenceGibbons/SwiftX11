//
//  XProtoConnectionWrapper.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/20/26.
//

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runs the existing C drain loop for a single client, but sets up the C++ transport bridge first.
void x11_run_connection_with_cpp_transport(int cfd);

#ifdef __cplusplus
}
#endif
