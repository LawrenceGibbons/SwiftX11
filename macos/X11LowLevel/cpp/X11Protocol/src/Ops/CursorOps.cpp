//
//  CursorOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/10/26.
//

#include "CursorOps.hpp"
#include "XProtoContext.hpp"
#include "ByteReader.hpp"
#include "CursorTable.hpp"
#include "Core/X11CoreOpcodes.hpp"

namespace x11 {

CursorOps::CursorOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::CreateCursor     , &CursorOps::onMajor, this); // CreateCursor
  reg.registerMajor(x11::opcode::CreateGlyphCursor, &CursorOps::onMajor, this); // CreateGlyphCursor
  reg.registerMajor(x11::opcode::FreeCursor       , &CursorOps::onMajor, this); // FreeCursor
  reg.registerMajor(x11::opcode::RecolorCursor    , &CursorOps::onMajor, this); // RecolorCursor
}

void CursorOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  auto* self = reinterpret_cast<CursorOps*>(user);
  self->handle(ctx, dc);
}

void CursorOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::CreateCursor     : handleCreateCursor(ctx, dc.seq, dc.br); return;
    case x11::opcode::CreateGlyphCursor: handleCreateGlyphCursor(ctx, dc.seq, dc.br); return;
    case x11::opcode::FreeCursor       : handleFreeCursor(ctx, dc.seq, dc.br); return;
    case x11::opcode::RecolorCursor    : handleRecolorCursor(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[CursorOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// 93 CreateCursor
// xCreateCursorReq:
//   cursor(CARD32), source(CARD32), mask(CARD32)
//   foreRed/Green/Blue (CARD16)
//   backRed/Green/Blue (CARD16)
//   x,y (CARD16)
// Total bytes after first 4 hdr: 28 (so remain should be 28)
void CursorOps::handleCreateCursor(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 28) { br.skip(br.remaining()); return; }

  const uint32_t cid  = br.readU32();
  const uint32_t src  = br.readU32();
  const uint32_t mask = br.readU32();

  CursorTable::RGB16 fg, bg;
  fg.r = br.readU16(); fg.g = br.readU16(); fg.b = br.readU16();
  bg.r = br.readU16(); bg.g = br.readU16(); bg.b = br.readU16();

  const uint16_t hotX = br.readU16();
  const uint16_t hotY = br.readU16();

  br.skip(br.remaining()); // consume any trailing

  ctx.cursors().createCursorPixmap(cid, src, mask, hotX, hotY, fg, bg);

#ifndef NDEBUG
  ctx.tracef("[CursorOps] CreateCursor cid=0x%08X src=0x%08X mask=0x%08X hot=%u,%u\n",
             (unsigned)cid, (unsigned)src, (unsigned)mask,
             (unsigned)hotX, (unsigned)hotY);
#endif
  // VOID: no reply
}

// 94 CreateGlyphCursor
// xCreateGlyphCursorReq:
//   cursor(CARD32), sourceFont(CARD32), maskFont(CARD32)
//   sourceChar(CARD16), maskChar(CARD16)
//   foreRGB (3*CARD16), backRGB (3*CARD16)
// Total after hdr: 28
void CursorOps::handleCreateGlyphCursor(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 28) { br.skip(br.remaining()); return; }

  const uint32_t cid  = br.readU32();
  const uint32_t sfon = br.readU32();
  const uint32_t mfon = br.readU32();

  const uint16_t srcChar = br.readU16();
  const uint16_t maskChar = br.readU16();

  CursorTable::RGB16 fg, bg;
  fg.r = br.readU16(); fg.g = br.readU16(); fg.b = br.readU16();
  bg.r = br.readU16(); bg.g = br.readU16(); bg.b = br.readU16();

  br.skip(br.remaining());

  ctx.cursors().createCursorGlyph(cid, sfon, mfon, srcChar, maskChar, fg, bg);

#ifndef NDEBUG
  ctx.tracef("[CursorOps] CreateGlyphCursor cid=0x%08X sfon=0x%08X mfon=0x%08X chars=%u/%u\n",
             (unsigned)cid, (unsigned)sfon, (unsigned)mfon,
             (unsigned)srcChar, (unsigned)maskChar);
#endif
  // VOID: no reply
}

// 95 FreeCursor
// xFreeCursorReq: cursor(CARD32) => remain 4
void CursorOps::handleFreeCursor(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t cid = br.readU32();
  br.skip(br.remaining());

  const bool removed = ctx.cursors().erase(cid);

#ifndef NDEBUG
  ctx.tracef("[CursorOps] FreeCursor cid=0x%08X removed=%d\n",
             (unsigned)cid, removed ? 1 : 0);
#endif
  // VOID: no reply
}

// 96 RecolorCursor
// xRecolorCursorReq:
//   cursor(CARD32)
//   foreRGB (3*CARD16)
//   backRGB (3*CARD16)
// remain 16
void CursorOps::handleRecolorCursor(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 16) { br.skip(br.remaining()); return; }
  const uint32_t cid = br.readU32();

  CursorTable::RGB16 fg, bg;
  fg.r = br.readU16(); fg.g = br.readU16(); fg.b = br.readU16();
  bg.r = br.readU16(); bg.g = br.readU16(); bg.b = br.readU16();

  br.skip(br.remaining());

  const bool ok = ctx.cursors().recolor(cid, fg, bg);

#ifndef NDEBUG
  ctx.tracef("[CursorOps] RecolorCursor cid=0x%08X ok=%d\n",
             (unsigned)cid, ok ? 1 : 0);
#endif
  // VOID: no reply
}

} // namespace x11
