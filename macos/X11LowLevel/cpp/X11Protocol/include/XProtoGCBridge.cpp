//
//  XProtoGCBridge.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/28/26.
//

#include "XProtoGCBridge.hpp"
#include "GCTable.hpp"

extern "C" int x11_proto_bridge_gc_get(uint32_t gc_xid, uint32_t* out_fg, uint32_t* out_bg)
{
  if (!out_fg || !out_bg) return 0;

  x11::GCState st{};
  if (!x11::GCTable::instance().find(gc_xid, st)) return 0;

  *out_fg = st.fg;
  *out_bg = st.bg;
  return 1;
}

