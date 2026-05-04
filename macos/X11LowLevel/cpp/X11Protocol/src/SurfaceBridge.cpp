//
//  SurfaceBridge.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/22/26.
//
//  C entrypoints for the host-pixel surface registry.
//
//  C3 (v1.19.35.48-dbg) flipped the ownership model: C++ now allocates
//  and frees the host buffers via DrawableSurfaceRegistry::ensure().
//  Swift no longer owns Foundation.Data buffers — it just requests sizes
//  via x11_surface_ensure() and reads via x11_server_copy_window_bgra().
//  The legacy x11_surface_update() is kept as a thin wrapper during the
//  transition (C3.1); C3.2 will switch the last Swift caller to
//  x11_surface_ensure() and the legacy entrypoint can then be deleted.
//

#include <cstdint>

#include "Core/XProtoServer.hpp"
#include "Core/SurfaceDesc.hpp"
#include "UI/UICommandQueue.hpp"
#include "XProtoServerBridge.h"
#include "Utils/TraceDefs.hpp"
#include "Utils/MachTime.hpp"

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
  s.ptr         = ptr;
  s.bytesPerRow = bytes_per_row;
  s.w           = w;
  s.h           = h;
  s.format      = x11::SurfaceFormat::XRGB8888;
  s.generation  = generation;

#if X11_TRACE_RESIZE_ENABLED
  TS_FPRINTF("[SURFACE_UPDATE] xid=0x%08X ptr=%p bpr=%u wh=%ux%u gen=%u prev=%ux%u sizeChanged=%d\n",
          (unsigned)host_xid, ptr, (unsigned)bytes_per_row,
          (unsigned)w, (unsigned)h, (unsigned)generation,
          hadPrev ? (unsigned)prev.w : 0u, hadPrev ? (unsigned)prev.h : 0u,
          (int)sizeChanged);
#endif

  // Legacy path: registry.set() takes the exclusive lock, allocates an
  // owned HostSurface internally if needed, and copies bytes from the
  // externally-provided ptr.  Swift may free its buffer immediately
  // after this call returns.
  srv->updateSurface(host_xid, s);

  if (sizeChanged) {
#if X11_TRACE_RESIZE_ENABLED
    TS_FPRINTF("[SURFACE_UPDATE] xid=0x%08X -> queueing SurfaceResized (prev=%ux%u new=%ux%u)\n",
            (unsigned)host_xid,
            hadPrev ? (unsigned)prev.w : 0u, hadPrev ? (unsigned)prev.h : 0u,
            (unsigned)w, (unsigned)h);
#endif
    x11_proto_bridge_surface_resized(host_xid);
  }
}

extern "C" uint32_t x11_surface_ensure(uint32_t host_xid,
                                       int32_t  w_px,
                                       int32_t  h_px)
{
  if (host_xid == 0 || w_px < 1 || h_px < 1) return 0;
  if (w_px > 65535 || h_px > 65535) return 0;

  x11::XProtoServer* srv = x11_proto_bridge_get_server();
  if (!srv) return 0;

  const uint16_t w = (uint16_t)w_px;
  const uint16_t h = (uint16_t)h_px;
  // 64-byte aligned stride.  All draw-op code uses bytesPerRow / 4 as
  // stridePixels, so this aligns rows to cache lines for free.
  const uint32_t bpr = ((uint32_t)w * 4u + 63u) & ~63u;

  // Check whether dimensions are actually changing (drives SurfaceResized).
  x11::SurfaceDesc prev{};
  const bool hadPrev = srv->ctx().surfaces().get(host_xid, prev);
  const bool sizeChanged = hadPrev && (prev.w != w || prev.h != h);

  const uint32_t gen = srv->ctx().surfaces().ensure(host_xid, w, h, bpr);
  if (gen == 0) {
#if X11_TRACE_RESIZE_ENABLED
    TS_FPRINTF("[SURFACE_ENSURE] xid=0x%08X wh=%ux%u FAILED to allocate\n",
               (unsigned)host_xid, (unsigned)w, (unsigned)h);
#endif
    return 0;
  }

#if X11_TRACE_RESIZE_ENABLED
  TS_FPRINTF("[SURFACE_ENSURE] xid=0x%08X wh=%ux%u bpr=%u gen=%u prev=%ux%u sizeChanged=%d\n",
             (unsigned)host_xid, (unsigned)w, (unsigned)h, (unsigned)bpr,
             (unsigned)gen,
             hadPrev ? (unsigned)prev.w : 0u, hadPrev ? (unsigned)prev.h : 0u,
             (int)sizeChanged);
#endif

  // Same downstream re-expose semantics as the legacy path.
  if (sizeChanged) {
    x11_proto_bridge_surface_resized(host_xid);
  }

  return gen;
}

extern "C" void x11_surface_clear(uint32_t host_xid)
{
  if (host_xid == 0) return;

  x11::XProtoServer* srv = x11_proto_bridge_get_server();
  if (!srv) return;

  srv->clearSurface(host_xid);
}
