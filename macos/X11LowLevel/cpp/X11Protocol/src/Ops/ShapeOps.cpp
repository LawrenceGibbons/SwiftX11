//
//  ShapeOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "ShapeOps.hpp"

#include <cmath>
#include <algorithm>

#include "XProtoContext.hpp"
#include "ByteReader.hpp"
#include "PixmapTable.hpp"
#include "GCTable.hpp"
#include "WindowTable.hpp"
#include "DrawableRW.hpp"

// bridge to C and Swift
#include "x11_requests.h"

// util
#include "Damage.hpp"

namespace x11 {

static inline float norm360(float a) {
  while (a < 0.0f)   a += 360.0f;
  while (a >= 360.0f) a -= 360.0f;
  return a;
}

static inline bool angle_in_arc(float theta, float start, float extent) {
  if (extent >= 0.0f) {
    const float end = norm360(start + extent);
    if (start <= end) return (theta >= start && theta <= end);
    return (theta >= start || theta <= end);
  } else {
    const float end = norm360(start + extent);
    if (end <= start) return (theta >= end && theta <= start);
    return (theta >= end || theta <= start);
  }
}

ShapeOps::ShapeOps(XProtoRegistrar& reg) {
  reg.registerMajor(68, &ShapeOps::onMajor, this); // PolyArc
  reg.registerMajor(70, &ShapeOps::onMajor, this); // PolyFillRectangle
  reg.registerMajor(71, &ShapeOps::onMajor, this); // PolyFillArc
}

void ShapeOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<ShapeOps*>(user)->handle(ctx, dc);
}

void ShapeOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case 68: handlePolyArc(ctx, dc.seq, dc.br); return;
    case 70: handlePolyFillRectangle(ctx, dc.seq, dc.br); return;
    case 71: handlePolyFillArc(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[ShapeOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// -----------------------------------------------------------------------------
// PolyFillRectangle (70)
// -----------------------------------------------------------------------------
  void ShapeOps::handlePolyFillRectangle(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
    // Request body after 4-byte header:
    //   CARD32 drawable
    //   CARD32 gc
    //   LISTofxRectangle (each 8 bytes: INT16 x, INT16 y, CARD16 w, CARD16 h)

    if (br.remaining() < 8) { br.skip(br.remaining()); return; }

    const uint32_t drawable = br.readU32();
    const uint32_t gcXid     = br.readU32();

    // Remaining bytes are rect list
    const std::size_t listBytes = br.remaining();
    const std::size_t nRects = listBytes / 8u;
    if (nRects == 0) { br.skip(br.remaining()); return; }

    // Resolve dst drawable -> writable 32bpp pixels
    // NOTE: This must reject depth-1 pixmaps (like your old resolve_drawable_pixels_rw did).
    DrawableRW dst{};
    if (!resolveDrawableRW(ctx, drawable, dst)) { br.skip(br.remaining()); return; }
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0) { br.skip(br.remaining()); return; }

    // GC fg
    uint32_t fg = 0xFF000000u;
    {
      GCState gst{};
      if (GCTable::instance().find(gcXid, gst)) fg = gst.fg;
    }

    // Fill
    for (std::size_t i = 0; i < nRects; i++) {
      const int16_t rx = (int16_t)br.readU16();
      const int16_t ry = (int16_t)br.readU16();
      const uint16_t rw = br.readU16();
      const uint16_t rh = br.readU16();

      if (rw == 0 || rh == 0) continue;

      int x0 = std::max(0, (int)rx);
      int y0 = std::max(0, (int)ry);
      int x1 = std::min((int)dst.w, (int)rx + (int)rw);
      int y1 = std::min((int)dst.h, (int)ry + (int)rh);
      if (x0 >= x1 || y0 >= y1) continue;

      for (int y = y0; y < y1; y++) {
        uint32_t* row = dst.pixels32 + (std::size_t)y * (std::size_t)dst.w;
        for (int x = x0; x < x1; x++) {
          row[x] = fg;
        }
      }
    }

    // consume any trailing bytes (if listBytes not multiple of 8)
    br.skip(br.remaining());

    // Present only if destination is a window (pixmaps get presented when copied to window)
    if (dst.is_window) {
      if (ctx.windows().isReadyToPresent(drawable)) {
        x11_requests_push_damage(drawable);
      } else {
        ctx.windows().markDirty(drawable);
      }
    }
  }
  
// -----------------------------------------------------------------------------
// PolyFillArc (71)
// -----------------------------------------------------------------------------
void ShapeOps::handlePolyFillArc(XProtoContext& ctx, uint16_t, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }

  const uint32_t drawable = br.readU32();
  const uint32_t gc_id    = br.readU32();

  // Resolve destination
  uint32_t* dstPixels = nullptr;
  int dstW = 0, dstH = 0;
  bool dstIsWindow = false;

  if (ctx.windows().exists(drawable)) {
    uint32_t* px = nullptr; uint32_t w=0,h=0;
    if (!x11_xproto_window_fb_rw(drawable, &px, &w, &h) || !px) {
      br.skip(br.remaining()); return;
    }
    dstPixels = px; dstW = (int)w; dstH = (int)h; dstIsWindow = true;
  } else {
    uint16_t pw=0, ph=0;
    dstPixels = ctx.pixmaps().mutablePixels(drawable, &pw, &ph);
    if (!dstPixels) { br.skip(br.remaining()); return; }
    dstW = (int)pw; dstH = (int)ph; dstIsWindow = false;
  }

  // GC fg
  uint32_t fg = 0xFF000000u;
  GCState st{};
  if (GCTable::instance().find(gc_id, st)) fg = st.fg;

  const size_t listBytes = br.remaining();
  const size_t narcs = listBytes / 12u;
  if (narcs == 0) { br.skip(br.remaining()); return; }

  for (size_t i = 0; i < narcs; i++) {
    const int16_t  ax = (int16_t)br.readU16();
    const int16_t  ay = (int16_t)br.readU16();
    const uint16_t aw = br.readU16();
    const uint16_t ah = br.readU16();
    const int16_t  a1 = (int16_t)br.readU16();
    const int16_t  a2 = (int16_t)br.readU16();

    if (aw == 0 || ah == 0) continue;

    const float start = (float)a1 / 64.0f;
    const float extent = (float)a2 / 64.0f;
    const bool full = std::fabs(extent) >= (360.0f - (1.0f/64.0f));

    const float rx = aw * 0.5f;
    const float ry = ah * 0.5f;
    const float cx = ax + rx;
    const float cy = ay + ry;

    int x0 = std::max(0, (int)ax);
    int y0 = std::max(0, (int)ay);
    int x1 = std::min((int)dstW, ax + (int)aw);
    int y1 = std::min((int)dstH, ay + (int)ah);
    if (x0 >= x1 || y0 >= y1) continue;

    for (int py = y0; py < y1; py++) {
      const float ny = ((float)py + 0.5f - cy) / ry;
      for (int px = x0; px < x1; px++) {
        const float nx = ((float)px + 0.5f - cx) / rx;
        if (nx*nx + ny*ny > 1.0f) continue;

        if (!full) {
          const float dx = (float)px + 0.5f - cx;
          const float dy = (float)py + 0.5f - cy;
          float theta = norm360(std::atan2(-dy, dx) * (180.0f / (float)M_PI));
          if (!angle_in_arc(theta, start, extent)) continue;
        }
        dstPixels[(size_t)py * (size_t)dstW + (size_t)px] = fg;
      }
    }
  }

  br.skip(br.remaining());
  if (dstIsWindow) {
    if (ctx.windows().isReadyToPresent(drawable)) {
      x11_requests_push_damage(drawable);
    } else {
      ctx.windows().markDirty(drawable);
    }
  }
}

// -----------------------------------------------------------------------------
// PolyArc (68) — outline
// -----------------------------------------------------------------------------
void ShapeOps::handlePolyArc(XProtoContext& ctx, uint16_t, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }

  const uint32_t drawable = br.readU32();
  const uint32_t gc_id    = br.readU32();

  uint32_t* dstPixels = nullptr;
  int dstW = 0, dstH = 0;
  bool dstIsWindow = false;

  if (ctx.windows().exists(drawable)) {
    uint32_t* px=nullptr; uint32_t w=0,h=0;
    if (!x11_xproto_window_fb_rw(drawable, &px, &w, &h) || !px) {
      br.skip(br.remaining()); return;
    }
    dstPixels = px; dstW=(int)w; dstH=(int)h; dstIsWindow=true;
  } else {
    uint16_t pw=0, ph=0;
    dstPixels = ctx.pixmaps().mutablePixels(drawable, &pw, &ph);
    if (!dstPixels) { br.skip(br.remaining()); return; }
    dstW=(int)pw; dstH=(int)ph; dstIsWindow=false;
  }

  uint32_t fg = 0xFF000000u;
  GCState st{};
  if (GCTable::instance().find(gc_id, st)) fg = st.fg;

  const size_t narcs = br.remaining() / 12u;
  if (narcs == 0) { br.skip(br.remaining()); return; }

  for (size_t i = 0; i < narcs; i++) {
    const int16_t  ax = (int16_t)br.readU16();
    const int16_t  ay = (int16_t)br.readU16();
    const uint16_t aw = br.readU16();
    const uint16_t ah = br.readU16();
    const int16_t  a1 = (int16_t)br.readU16();
    const int16_t  a2 = (int16_t)br.readU16();

    if (aw == 0 || ah == 0) continue;

    const float start = (float)a1 / 64.0f;
    const float extent = (float)a2 / 64.0f;
    const bool full = std::fabs(extent) >= (360.0f - (1.0f/64.0f));

    const float rx = aw * 0.5f;
    const float ry = ah * 0.5f;
    const float cx = ax + rx;
    const float cy = ay + ry;

    int x0 = std::max(0, (int)ax);
    int y0 = std::max(0, (int)ay);
    int x1 = std::min(dstW, ax + (int)aw);
    int y1 = std::min(dstH, ay + (int)ah);

    const float eps = std::max(1.0f / std::max(rx, 1.0f),
                               1.0f / std::max(ry, 1.0f));

    for (int py = y0; py < y1; py++) {
      const float ny = ((float)py + 0.5f - cy) / ry;
      for (int px = x0; px < x1; px++) {
        const float nx = ((float)px + 0.5f - cx) / rx;
        if (std::fabs(nx*nx + ny*ny - 1.0f) > eps) continue;

        if (!full) {
          const float dx = (float)px + 0.5f - cx;
          const float dy = (float)py + 0.5f - cy;
          float theta = norm360(std::atan2(-dy, dx) * (180.0f / (float)M_PI));
          if (!angle_in_arc(theta, start, extent)) continue;
        }
        dstPixels[(size_t)py * (size_t)dstW + (size_t)px] = fg;
      }
    }
  }

  br.skip(br.remaining());
  if (dstIsWindow) {
    if (ctx.windows().isReadyToPresent(drawable)) {
      x11_requests_push_damage(drawable);
    } else {
      ctx.windows().markDirty(drawable);
    }
  }
}

} // namespace x11
