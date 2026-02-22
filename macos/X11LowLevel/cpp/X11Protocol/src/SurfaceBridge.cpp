//
//  SurfaceBridge.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/22/26.
//
//  Temporary C entrypoints for Swift-owned surfaces.
//  Swift calls x11_surface_update/clear; C++ installs SurfaceDesc into SurfaceRegistry.
//

#include <cstdint>

#include "Core/XProtoServer.hpp"
#include "Core/SurfaceDesc.hpp"

// We need access to the current server instance managed by XProtoServerBridge.cpp.
// This accessor is defined in XProtoServerBridge.cpp (see snippet below).
extern "C" x11::XProtoServer* x11_proto_bridge_get_server(void);

extern "C" void x11_surface_update(uint32_t host_xid,
                                   void* ptr,
                                   uint32_t bytes_per_row,
                                   uint16_t w,
                                   uint16_t h,
                                   uint32_t generation)
{
  if (host_xid == 0 || !ptr || bytes_per_row == 0 || w == 0 || h == 0) return;

  x11::XProtoServer* srv = x11_proto_bridge_get_server();
  if (!srv) return;

  x11::SurfaceDesc s{};
  s.ptr = ptr;
  s.bytesPerRow = bytes_per_row;
  s.w = w;
  s.h = h;
  s.format = x11::SurfaceFormat::XRGB8888;  // keep aligned with your SurfaceDesc definition
  s.generation = generation;

  srv->updateSurface(host_xid, s);
}

extern "C" void x11_surface_clear(uint32_t host_xid)
{
  if (host_xid == 0) return;

  x11::XProtoServer* srv = x11_proto_bridge_get_server();
  if (!srv) return;

  srv->clearSurface(host_xid);
}
