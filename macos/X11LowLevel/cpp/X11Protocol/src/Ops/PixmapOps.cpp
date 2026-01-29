//
//  PixmapOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/29/26.
//


#include "PixmapOps.hpp"

#include "XProtoContext.hpp"
#include "ByteReader.hpp"
#include "PixmapTable.hpp"

namespace x11 {

PixmapOps::PixmapOps(XProtoRegistrar& reg) {
  reg.registerMajor(53, &PixmapOps::onMajor, this); // CreatePixmap
  reg.registerMajor(54, &PixmapOps::onMajor, this); // FreePixmap
}

void PixmapOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<PixmapOps*>(user)->handle(ctx, dc);
}

void PixmapOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case 53: handleCreatePixmap(ctx, dc.minor /*depth*/, dc.br); return;
    case 54: handleFreePixmap(ctx, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[PixmapOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// ------------------------------------------------------------
// major 53: CreatePixmap
// Request body after 4-byte header (12 bytes):
//   CARD32 pid
//   CARD32 drawable   (unused for bring-up)
//   CARD16 width
//   CARD16 height
// Depth is in the request header's "minor" byte.
// ------------------------------------------------------------
void PixmapOps::handleCreatePixmap(XProtoContext& ctx, uint8_t depth, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }

  const uint32_t pid      = br.readU32();
  (void)br.readU32(); // drawable (unused)
  const uint16_t w        = br.readU16();
  const uint16_t h        = br.readU16();
  br.skip(br.remaining());

  if (pid == 0) return;

  // Match old behavior: clamp w/h to at least 1.
  const uint16_t ww = (w == 0) ? 1 : w;
  const uint16_t hh = (h == 0) ? 1 : h;

  ctx.pixmaps().createPixmap(pid, depth, ww, hh);

#ifndef NDEBUG
  ctx.tracef("[PixmapOps] CreatePixmap pid=0x%08X depth=%u %ux%u\n",
             (unsigned)pid, (unsigned)depth, (unsigned)ww, (unsigned)hh);
#endif
}

// ------------------------------------------------------------
// major 54: FreePixmap
// Request body after 4-byte header (4 bytes):
//   CARD32 pid
// ------------------------------------------------------------
void PixmapOps::handleFreePixmap(XProtoContext& ctx, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }

  const uint32_t pid = br.readU32();
  br.skip(br.remaining());

  if (pid == 0) return;

  ctx.pixmaps().freePixmap(pid);

#ifndef NDEBUG
  ctx.tracef("[PixmapOps] FreePixmap pid=0x%08X\n", (unsigned)pid);
#endif
}

} // namespace x11
