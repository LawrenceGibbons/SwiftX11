//
//  x11_window_set_mapped.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/26/26.
//

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
  
  void x11_xproto_c_window_set_mapped(uint32_t xid, int mapped);
  void x11_xproto_c_window_set_presentable(uint32_t xid, int presentable);
  
#ifdef __cplusplus
}
#endif
