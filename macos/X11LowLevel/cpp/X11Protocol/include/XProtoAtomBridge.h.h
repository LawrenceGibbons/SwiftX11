//
//  XProtoAtomBridge.h.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Intern atom name -> atom id.
// only_if_exists: 1 => return 0 if missing, 0 => create if missing.
uint32_t x11_xproto_atoms_intern(const char* name, uint32_t name_len, int only_if_exists);

// Lookup atom id -> name pointer + length.
// Returned pointer must remain valid until next call or forever (your choice).
// name_len_out is required.
void x11_xproto_atoms_name(uint32_t atom, const char** name_out, uint32_t* name_len_out);

#ifdef __cplusplus
}
#endif
