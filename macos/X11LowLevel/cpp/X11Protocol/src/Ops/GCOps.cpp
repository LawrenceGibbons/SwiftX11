#include "Ops/GCOps.hpp"

#include "Core/XProtoContext.hpp"
#include "Utils/ByteReader.hpp"
#include "Core/GCTable.hpp"
#include "Core/FontTable.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Utils/MachTime.hpp"

namespace x11 {

GCOps::GCOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::CreateGC,          &GCOps::onMajor, this); // 55
  reg.registerMajor(x11::opcode::ChangeGC,          &GCOps::onMajor, this); // 56
  reg.registerMajor(x11::opcode::CopyGC,            &GCOps::onMajor, this); // 57
  reg.registerMajor(x11::opcode::SetDashes,          &GCOps::onMajor, this); // 58
  reg.registerMajor(x11::opcode::SetClipRectangles, &GCOps::onMajor, this); // 59
  reg.registerMajor(x11::opcode::FreeGC,            &GCOps::onMajor, this); // 60
}

void GCOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<GCOps*>(user)->handle(ctx, dc);
}

void GCOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::CreateGC:          handleCreateGC(ctx, dc.seq, dc.br); return;
    case x11::opcode::ChangeGC:          handleChangeGC(ctx, dc.seq, dc.br); return;
    case x11::opcode::CopyGC:            handleCopyGC(ctx, dc.seq, dc.br); return;
    case x11::opcode::SetDashes:          handleSetDashes(ctx, dc.seq, dc.br); return;
    case x11::opcode::SetClipRectangles: handleSetClipRectangles(ctx, dc.seq, dc.minor, dc.br); return;
    case x11::opcode::FreeGC:            handleFreeGC(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[GCOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// Keep your bring-up mapping rules.
uint32_t GCOps::mapPixelToARGB(uint32_t val) {
  if (val == 0) return 0xFF000000u;
  if (val == 1) return 0xFFFFFFFFu;
  return 0xFF000000u | (val & 0x00FFFFFFu);
}

// bits:
//  0 GCFunction        2 GCForeground       14 GCFont
//  1 GCPlaneMask       3 GCBackground       15 GCSubwindowMode
// 16 GCGraphicsExposures  17 GCClipXOrigin  18 GCClipYOrigin
// 19 GCClipMask (PIXMAP or None=0)
void GCOps::applyValueMask(uint32_t vmask, ByteReader& br, GCState& st)
{
  for (uint32_t bit = 0; bit < 23; bit++) {
    if ((vmask & (1u << bit)) == 0) continue;
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t val = br.readU32();

    switch (bit) {
      case 0:  st.function   = (uint8_t)(val & 0xFFu); break;
      case 1:  st.plane_mask = val; break;
      case 2:  st.fg         = mapPixelToARGB(val); break;
      case 3:  st.bg         = mapPixelToARGB(val); break;
      case 4:  st.line_width = (uint16_t)(val & 0xFFFFu); break;
      case 5:  st.line_style = (uint8_t)(val & 0x03u); break;
      case 6:  st.cap_style  = (uint8_t)(val & 0x03u); break;
      case 7:  st.join_style = (uint8_t)(val & 0x03u); break;
      case 8:  st.fill_style = (uint8_t)(val & 0x03u); break;
      case 9:  st.fill_rule  = (uint8_t)(val & 0x01u); break;
      case 10: st.tile       = val; break;
      case 11: st.stipple    = val; break;
      case 12: st.ts_x_origin = (int16_t)(val & 0xFFFFu); break;
      case 13: st.ts_y_origin = (int16_t)(val & 0xFFFFu); break;
      case 14: st.font       = val; break;
      case 15: st.subwindow_mode = (uint8_t)(val & 0x01u); break;
      case 16: st.graphics_exposures = (val != 0); break;
      case 17: st.clip_x_origin = (int16_t)(val & 0xFFFFu); break;
      case 18: st.clip_y_origin = (int16_t)(val & 0xFFFFu); break;
      case 19:
        // clip-mask: None (0) clears clip; non-zero = pixmap (not yet implemented)
        if (val == 0) { st.has_clip = false; st.clip_rects.clear(); }
        break;
      case 20: st.dash_offset   = (uint16_t)(val & 0xFFFFu); break;
      case 21: st.dashes_single = (uint8_t)(val & 0xFFu); break;
      case 22: st.arc_mode      = (uint8_t)(val & 0x01u); break;
      default: break;
    }
  }
}
  
// major 55 CreateGC
  void GCOps::handleCreateGC(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
    // Body:
    //   CARD32 gc
    //   CARD32 drawable
    //   CARD32 valueMask
    //   LISTofCARD32 values
    if (br.remaining() < 12) { br.skip(br.remaining()); return; }
    
    const uint32_t gcXid    = br.readU32();
    (void)br.readU32(); // drawable (unused)
    const uint32_t vmask    = br.readU32();
    
    auto st = GCTable::instance().getOrCreate(gcXid);
#ifdef X11_TRACE_VERBOSE
    const uint32_t oldFont = st.font;
#endif

    applyValueMask(vmask, br, st);
    // Consume any trailing padding (CreateGC has no fixed padding, but callers may pass more bytes)
    br.skip(br.remaining());

    GCTable::instance().upsert(st);

#ifdef X11_TRACE_VERBOSE
    if (st.font != oldFont) {
      const x11::font::BdfFont* ff = ctx.fonts().get(st.font);
      TS_FPRINTF("[GCOps] GCFont gc=0x%08X font=0x%08X resolved=\"%s\"\n",
              (unsigned)gcXid, (unsigned)st.font,
              ff ? ff->name.c_str() : "<unresolved>");
    }
#endif
  }

// major 56 ChangeGC
  void GCOps::handleChangeGC(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
    // Body:
    //   CARD32 gc
    //   CARD32 valueMask
    //   LISTofCARD32 values
    if (br.remaining() < 8) { br.skip(br.remaining()); return; }

    const uint32_t gcXid = br.readU32();
    const uint32_t vmask = br.readU32();

    auto st = GCTable::instance().getOrCreate(gcXid);
#ifdef X11_TRACE_VERBOSE
    const uint32_t oldFont = st.font;
#endif

    applyValueMask(vmask, br, st);
    br.skip(br.remaining());

    GCTable::instance().upsert(st);

#ifdef X11_TRACE_VERBOSE
    if (st.font != oldFont) {
      const x11::font::BdfFont* ff = ctx.fonts().get(st.font);
      TS_FPRINTF("[GCOps] GCFont gc=0x%08X font=0x%08X resolved=\"%s\"\n",
              (unsigned)gcXid, (unsigned)st.font,
              ff ? ff->name.c_str() : "<unresolved>");
    }
#endif
  }

// major 57 CopyGC
void GCOps::handleCopyGC(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  // Body: CARD32 srcGC, CARD32 dstGC, CARD32 valueMask
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }
  const uint32_t srcGcXid = br.readU32();
  const uint32_t dstGcXid = br.readU32();
  const uint32_t vmask    = br.readU32();
  br.skip(br.remaining());

  auto src = GCTable::instance().getOrCreate(srcGcXid);
  auto dst = GCTable::instance().getOrCreate(dstGcXid);

  if (vmask & (1u << 0))  dst.function   = src.function;
  if (vmask & (1u << 1))  dst.plane_mask = src.plane_mask;
  if (vmask & (1u << 2))  dst.fg         = src.fg;
  if (vmask & (1u << 3))  dst.bg         = src.bg;
  if (vmask & (1u << 4))  dst.line_width = src.line_width;
  if (vmask & (1u << 5))  dst.line_style = src.line_style;
  if (vmask & (1u << 6))  dst.cap_style  = src.cap_style;
  if (vmask & (1u << 7))  dst.join_style = src.join_style;
  if (vmask & (1u << 8))  dst.fill_style = src.fill_style;
  if (vmask & (1u << 9))  dst.fill_rule  = src.fill_rule;
  if (vmask & (1u << 10)) dst.tile       = src.tile;
  if (vmask & (1u << 11)) dst.stipple    = src.stipple;
  if (vmask & (1u << 12)) dst.ts_x_origin = src.ts_x_origin;
  if (vmask & (1u << 13)) dst.ts_y_origin = src.ts_y_origin;
  if (vmask & (1u << 14)) dst.font       = src.font;
  if (vmask & (1u << 15)) dst.subwindow_mode = src.subwindow_mode;
  if (vmask & (1u << 16)) dst.graphics_exposures = src.graphics_exposures;
  if (vmask & (1u << 17)) dst.clip_x_origin = src.clip_x_origin;
  if (vmask & (1u << 18)) dst.clip_y_origin = src.clip_y_origin;
  if (vmask & (1u << 19)) { dst.has_clip = src.has_clip; dst.clip_rects = src.clip_rects; }
  if (vmask & (1u << 20)) dst.dash_offset   = src.dash_offset;
  if (vmask & (1u << 21)) dst.dashes_single = src.dashes_single;
  if (vmask & (1u << 22)) dst.arc_mode      = src.arc_mode;

  GCTable::instance().upsert(dst);
  ctx.tracef("[GCOps] CopyGC src=0x%08X dst=0x%08X vmask=0x%08X\n",
             (unsigned)srcGcXid, (unsigned)dstGcXid, (unsigned)vmask);
}

// major 58 SetDashes
void GCOps::handleSetDashes(XProtoContext& /*ctx*/, uint16_t /*seq*/, ByteReader& br) {
  // Wire format: CARD32 gc + CARD16 dashOffset + CARD16 nDashes + nDashes bytes (padded to 4)
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }
  const uint32_t gcXid      = br.readU32();
  const uint16_t dashOffset = br.readU16();
  const uint16_t nDashes    = br.readU16();

  auto st = GCTable::instance().getOrCreate(gcXid);
  st.dash_offset = dashOffset;
  st.dash_list.clear();
  st.dash_list.reserve(nDashes);
  for (uint16_t i = 0; i < nDashes && br.remaining() > 0; i++) {
    st.dash_list.push_back(br.readU8());
  }
  br.skip(br.remaining()); // consume padding
  GCTable::instance().upsert(st);
}

// major 59 SetClipRectangles
void GCOps::handleSetClipRectangles(XProtoContext& ctx, uint16_t /*seq*/, uint8_t ordering, ByteReader& br) {
  // Wire format (after 4-byte header):
  //   CARD32 gc
  //   INT16  clip-x-origin
  //   INT16  clip-y-origin
  //   LISTofRECTANGLE  (each 8 bytes: INT16 x, INT16 y, CARD16 w, CARD16 h)
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }

  const uint32_t gcXid = br.readU32();
  const int16_t  cxo   = br.readI16();
  const int16_t  cyo   = br.readI16();

  auto st = GCTable::instance().getOrCreate(gcXid);
  st.clip_x_origin = cxo;
  st.clip_y_origin = cyo;
  st.has_clip = true;
  st.clip_rects.clear();

  const size_t nRects = br.remaining() / 8u;
  st.clip_rects.reserve(nRects);
  for (size_t i = 0; i < nRects; i++) {
    if (br.remaining() < 8) break;
    ClipRect r;
    r.x = br.readI16();
    r.y = br.readI16();
    r.w = br.readU16();
    r.h = br.readU16();
    st.clip_rects.push_back(r);
  }
  br.skip(br.remaining());
  GCTable::instance().upsert(st);

  (void)ordering; // stored order hint; we don't optimize by ordering yet
}

// major 60 FreeGC
void GCOps::handleFreeGC(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t gcXid = br.readU32();
  br.skip(br.remaining());

  GCTable::instance().erase(gcXid);
  // ctx.tracef("[GCOps] FreeGC gc=0x%08X\n", (unsigned)gcXid);
}

} // namespace x11
