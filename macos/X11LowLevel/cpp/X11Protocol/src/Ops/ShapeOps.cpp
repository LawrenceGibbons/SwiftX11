//
//  ShapeOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "Ops/ShapeOps.hpp"

#include <cmath>
#include <algorithm>

#include "Core/XProtoContext.hpp"
#include "Utils/ByteReader.hpp"
#include "Core/PixmapTable.hpp"
#include "Core/GCTable.hpp"
#include "Core/WindowTable.hpp"
#include "Core/DrawableRW.hpp"
#include "Core/X11CoreOpcodes.hpp"

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
  reg.registerMajor(x11::opcode::PolyArc,           &ShapeOps::onMajor, this); // PolyArc
  reg.registerMajor(x11::opcode::PolyFillRectangle, &ShapeOps::onMajor, this); // PolyFillRectangle
  reg.registerMajor(x11::opcode::PolyFillArc      , &ShapeOps::onMajor, this); // PolyFillArc
}

void ShapeOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<ShapeOps*>(user)->handle(ctx, dc);
}

void ShapeOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::PolyArc          : handlePolyArc(ctx, dc.seq, dc.br); return;
    case x11::opcode::PolyFillRectangle: handlePolyFillRectangle(ctx, dc.seq, dc.br); return;
    case x11::opcode::PolyFillArc      : handlePolyFillArc(ctx, dc.seq, dc.br); return;
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
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }
    
    const uint32_t drawable = br.readU32();
    const uint32_t gcXid    = br.readU32();
    
    const std::size_t nRects = br.remaining() / 8u;
    
#ifndef NDEBUG
  if (drawable == 0x10000012u) {
    fprintf(stderr,
            "[PolyFillRect] dst=0x%08X gc=0x%08X nrect=%u\n", // first=(%d,%d %u×%u)\n",
            (unsigned)drawable, (unsigned)gcXid,
            (unsigned)nRects);
//            (int)rects[0].x, (int)rects[0].y);
//            (unsigned)rects[0].w, (unsigned)rects[0].h);
  }
#endif
    
    if (nRects == 0) { br.skip(br.remaining()); return; }
    
    DrawableRW dst{};
    if (!resolveDrawableRW(ctx, drawable, dst)) { br.skip(br.remaining()); return; }
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0) { br.skip(br.remaining()); return; }
    
    uint32_t fg = 0xFF000000u;
    {
      GCState gst{};
      if (GCTable::instance().find(gcXid, gst)) fg = gst.fg;
    }
    
    for (std::size_t i = 0; i < nRects; i++) {
      const int16_t rx = br.readI16();
      const int16_t ry = br.readI16();
      const uint16_t rw = br.readU16();
      const uint16_t rh = br.readU16();

#ifndef NDEBUG
      if ( drawable == 0x10000012u && i == 0 ) {
        fprintf(stderr,
                "[PolyFillRect] continued, first=(%d,%d %u×%u)\n",
                (int)rx, (int)ry,
                (unsigned)rw, (unsigned)rh);
        
      }
#endif

      if (rw == 0 || rh == 0) continue;
      
      const int32_t rx0 = rx;
      const int32_t ry0 = ry;
      const int32_t rx1 = rx0 + (int32_t)rw;
      const int32_t ry1 = ry0 + (int32_t)rh;
      
      int32_t x0 = std::max<int32_t>(0, rx0);
      int32_t y0 = std::max<int32_t>(0, ry0);
      int32_t x1 = std::min<int32_t>((int32_t)dst.w, rx1);
      int32_t y1 = std::min<int32_t>((int32_t)dst.h, ry1);
      if (x0 >= x1 || y0 >= y1) continue;
      
      for (int32_t y = y0; y < y1; y++) {
        uint32_t* row = dst.pixels32 + (std::size_t)y * (std::size_t)dst.w;
        for (int32_t x = x0; x < x1; x++) row[(size_t)x] = fg;
      }
    }
    
    br.skip(br.remaining());
    
    // Only present if destination is a window.
    if (dst.isWindow) {
      damageOrDirty(ctx, drawable);
    }
  }
  
// -----------------------------------------------------------------------------
// PolyFillArc (71)
// -----------------------------------------------------------------------------
  void ShapeOps::handlePolyFillArc(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }

    const uint32_t drawable = br.readU32();
    const uint32_t gc_id    = br.readU32();

    const std::size_t narcs = br.remaining() / 12u;
    if (narcs == 0) { br.skip(br.remaining()); return; }

    DrawableRW dst{};
    if (!resolveDrawableRW(ctx, drawable, dst)) { br.skip(br.remaining()); return; }
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0) { br.skip(br.remaining()); return; }

    // GC fg
    uint32_t fg = 0xFF000000u;
    {
      GCState st{};
      if (GCTable::instance().find(gc_id, st)) fg = st.fg;
    }

    const int dstW = (int)dst.w;
    const int dstH = (int)dst.h;
    uint32_t* dstPixels = dst.pixels32;

    for (std::size_t i = 0; i < narcs; i++) {
      const int16_t  ax = br.readI16();
      const int16_t  ay = br.readI16();
      const uint16_t aw = br.readU16();
      const uint16_t ah = br.readU16();
      const int16_t  a1 = br.readI16();
      const int16_t  a2 = br.readI16();

      if (aw == 0 || ah == 0) continue;

      const float start  = (float)a1 / 64.0f;
      const float extent = (float)a2 / 64.0f;
      const bool full = std::fabs(extent) >= (360.0f - (1.0f / 64.0f));

      const float rx = (float)aw * 0.5f;
      const float ry = (float)ah * 0.5f;
      if (rx <= 0.0f || ry <= 0.0f) continue;

      const float cx = (float)ax + rx;
      const float cy = (float)ay + ry;

      int x0 = std::max(0, (int)ax);
      int y0 = std::max(0, (int)ay);
      int x1 = std::min(dstW, (int)ax + (int)aw);
      int y1 = std::min(dstH, (int)ay + (int)ah);
      if (x0 >= x1 || y0 >= y1) continue;

      for (int py = y0; py < y1; py++) {
        const float ny = (((float)py + 0.5f) - cy) / ry;
        for (int px = x0; px < x1; px++) {
          const float nx = (((float)px + 0.5f) - cx) / rx;
          if (nx*nx + ny*ny > 1.0f) continue;

          if (!full) {
            const float dx = ((float)px + 0.5f) - cx;
            const float dy = ((float)py + 0.5f) - cy;
            const float theta = norm360(std::atan2(-dy, dx) * (180.0f / (float)M_PI));
            if (!angle_in_arc(theta, start, extent)) continue;
          }

          dstPixels[(size_t)py * (size_t)dstW + (size_t)px] = fg;
        }
      }
    }

    br.skip(br.remaining());

    // Only present if destination is a window.
    if (dst.isWindow) {
      damageOrDirty(ctx, drawable);
    }
  }

// -----------------------------------------------------------------------------
// PolyArc (68) — outline
// -----------------------------------------------------------------------------
  void ShapeOps::handlePolyArc(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }

    const uint32_t drawable = br.readU32();
    const uint32_t gc_id    = br.readU32();

    const std::size_t narcs = br.remaining() / 12u;
    if (narcs == 0) { br.skip(br.remaining()); return; }

    DrawableRW dst{};
    if (!resolveDrawableRW(ctx, drawable, dst)) { br.skip(br.remaining()); return; }
    if (!dst.pixels32 || dst.w == 0 || dst.h == 0) { br.skip(br.remaining()); return; }

    // GC fg
    uint32_t fg = 0xFF000000u;
    {
      GCState st{};
      if (GCTable::instance().find(gc_id, st)) fg = st.fg;
    }

    const int dstW = (int)dst.w;
    const int dstH = (int)dst.h;
    uint32_t* dstPixels = dst.pixels32;

    for (std::size_t i = 0; i < narcs; i++) {
      const int16_t  ax = br.readI16();
      const int16_t  ay = br.readI16();
      const uint16_t aw = br.readU16();
      const uint16_t ah = br.readU16();
      const int16_t  a1 = br.readI16();
      const int16_t  a2 = br.readI16();
      if (aw == 0 || ah == 0) continue;

      const float start  = (float)a1 / 64.0f;
      const float extent = (float)a2 / 64.0f;
      const bool full = std::fabs(extent) >= (360.0f - (1.0f / 64.0f));

      const float rx = (float)aw * 0.5f;
      const float ry = (float)ah * 0.5f;
      if (rx <= 0.0f || ry <= 0.0f) continue;

      const float cx = (float)ax + rx;
      const float cy = (float)ay + ry;

      int x0 = std::max(0, (int)ax);
      int y0 = std::max(0, (int)ay);
      int x1 = std::min(dstW, (int)ax + (int)aw);
      int y1 = std::min(dstH, (int)ay + (int)ah);
      if (x0 >= x1 || y0 >= y1) continue;

      const float eps = std::max(1.0f / std::max(rx, 1.0f),
                                 1.0f / std::max(ry, 1.0f));

      for (int py = y0; py < y1; py++) {
        const float ny = (((float)py + 0.5f) - cy) / ry;
        for (int px = x0; px < x1; px++) {
          const float nx = (((float)px + 0.5f) - cx) / rx;

          // Thin ring test
          if (std::fabs(nx*nx + ny*ny - 1.0f) > eps) continue;

          if (!full) {
            const float dx = ((float)px + 0.5f) - cx;
            const float dy = ((float)py + 0.5f) - cy;
            const float theta = norm360(std::atan2(-dy, dx) * (180.0f / (float)M_PI));
            if (!angle_in_arc(theta, start, extent)) continue;
          }

          dstPixels[(size_t)py * (size_t)dstW + (size_t)px] = fg;
        }
      }
    }

    br.skip(br.remaining());

    if (dst.isWindow) {
      damageOrDirty(ctx, drawable);
    }
  }
  
} // namespace x11
