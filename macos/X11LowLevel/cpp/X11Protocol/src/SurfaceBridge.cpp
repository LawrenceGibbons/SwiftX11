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
#include "UI/UICommandQueue.hpp"
#include "XProtoServerBridge.h"

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

  // Check if the surface dimensions are changing (resize vs first registration).
  x11::SurfaceDesc prev{};
  bool hadPrev = srv->ctx().surfaces().get(host_xid, prev);
  bool sizeChanged = hadPrev && (prev.w != w || prev.h != h);

  x11::SurfaceDesc s{};
  s.ptr = ptr;
  s.bytesPerRow = bytes_per_row;
  s.w = w;
  s.h = h;
  s.format = x11::SurfaceFormat::XRGB8888;
  s.generation = generation;

  fprintf(stderr, "[SURFACE_UPDATE] xid=0x%08X ptr=%p bpr=%u wh=%ux%u gen=%u prev=%ux%u sizeChanged=%d\n",
          (unsigned)host_xid, ptr, (unsigned)bytes_per_row,
          (unsigned)w, (unsigned)h, (unsigned)generation,
          hadPrev ? (unsigned)prev.w : 0u, hadPrev ? (unsigned)prev.h : 0u,
          (int)sizeChanged);

  srv->updateSurface(host_xid, s);

  // If the surface grew (resize after initial registration), re-expose the
  // entire subtree.  This is critical because the first sendExposeSubtree
  // (at SetPresentable time) may have run when the surface was still at its
  // initial (too-small) dimensions.  Child windows at far offsets (e.g.,
  // xterm's scrollbar at the right edge) would have been clipped to zero
  // and never rendered.  Triggering a new expose round-trip now that the
  // surface is at the correct size lets those children actually draw.
  if (sizeChanged) {
    fprintf(stderr, "[SURFACE_UPDATE] xid=0x%08X -> queueing SurfaceResized re-expose\n",
            (unsigned)host_xid);
    x11_proto_bridge_surface_resized(host_xid);
  }
}

extern "C" void x11_surface_clear(uint32_t host_xid)
{
  if (host_xid == 0) return;

  x11::XProtoServer* srv = x11_proto_bridge_get_server();
  if (!srv) return;

  srv->clearSurface(host_xid);
}
