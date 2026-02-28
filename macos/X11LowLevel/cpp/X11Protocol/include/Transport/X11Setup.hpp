//
//  X11Setup.hpp
//  X11LowLevel
//
//  X11 connection setup handshake functions.
//  Moved from x11_xproto.c → C++ (extern "C" linkage preserved).
//

#pragma once
#include <cstdint>

extern "C" {

void x11_send_setup_failed_le(int fd, const char* msg);
void x11_send_setup_success_minimal_little_endian(int fd,
                                                  uint32_t rid_base,
                                                  uint32_t rid_mask);

} // extern "C"
