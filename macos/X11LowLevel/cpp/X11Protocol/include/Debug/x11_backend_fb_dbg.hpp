//
//  x11_backend_fb_dbg.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 2/14/26.
//

#pragma once
#include "x11_backend_fb.h"

#define FB_RESIZE(wid,w,h,why) \
  x11_backend_fb_resize_dbg((wid),(w),(h),(why),__FILE__,__LINE__)

#define FB_CREATE(wid,w,h,owner_fd,out_dirty,why) \
  x11_backend_fb_create_slot_dbg((wid),(w),(h),(owner_fd),(out_dirty),(why),__FILE__,__LINE__)
