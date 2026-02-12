//
//  FontOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#include "Ops/FontOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Utils/ByteReader.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Core/XProtoTypes.hpp"
#include "Core/XProtoWire.hpp"
#include "Utils/WireLE.hpp"
#include "Core/FontTable.hpp"
#include "Fonts/BDF.hpp"
#include "Core/AtomTable.hpp"
#include <vector>
#include <cstring>
#include "Utils/WireLE.hpp"      // for wr16_le / wr32_le if needed
#include <X11/Xmd.h>    // copy of .../X11/include/X11/Xproto.h
#include <X11/Xproto.h>    // copy of .../X11/include/X11/Xproto.h
#include "Core/X11CoreOpcodes.hpp"

#include <cstring>   // strlen
#include <unordered_set>
#include <vector>

namespace x11 {

#pragma pack(push,1)
struct xCharInfo {
  int16_t  leftSideBearing;
  int16_t  rightSideBearing;
  int16_t  characterWidth;
  int16_t  ascent;
  int16_t  descent;
  uint16_t attributes;
};

struct xQueryFontReplyFixed {
  // This is the part AFTER the standard 32-byte reply header.
  // The standard 32-byte reply header already includes:
  //  - response type, pad, seq, length
  //  - minBounds (xCharInfo)
  //  - maxBounds (xCharInfo)
  //
  // What remains in the fixed reply struct is:
  uint16_t minCharOrByte2;
  uint16_t maxCharOrByte2;
  uint16_t defaultChar;
  uint16_t nFontProps;
  uint8_t  drawDirection;
  uint8_t  minByte1;
  uint8_t  maxByte1;
  uint8_t  allCharsExist;
  int16_t  fontAscent;
  int16_t  fontDescent;
  uint32_t nCharInfos;
};
#pragma pack(pop)  

FontOps::FontOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::OpenFont        , &FontOps::onMajor, this); // OpenFont
  reg.registerMajor(x11::opcode::CloseFont       , &FontOps::onMajor, this); // CloseFont
  reg.registerMajor(x11::opcode::QueryFont       , &FontOps::onMajor, this); // QueryFont
  reg.registerMajor(x11::opcode::QueryTextExtents, &FontOps::onMajor, this); // QueryTextExtents
  reg.registerMajor(x11::opcode::QueryBestSize   , &FontOps::onMajor, this); // QueryBestSize
}

void FontOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  static_cast<FontOps*>(user)->handle(ctx, dc);
}

void FontOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::OpenFont        : handleOpenFont(ctx, dc.seq, dc.br); return;
    case x11::opcode::CloseFont       : handleCloseFont(ctx, dc.seq, dc.br); return;
    case x11::opcode::QueryFont       : handleQueryFont(ctx, dc.seq, dc.br); return;
    case x11::opcode::QueryTextExtents: {
      // x11::opcode::QueryBestSize   oddLength is “odd” bit from the request header’s minor opcode in some encodings;
      // in core protocol QueryTextExtents uses the “odd length” bit in the request.
      // Your dispatcher provides dc.minor; use its LSB.
      const bool odd = (dc.minor & 1u) != 0;
      handleQueryTextExtents(ctx, dc.seq, odd, dc.br);
      return;
    }
    case x11::opcode::QueryBestSize   : handleQueryBestSize(ctx, dc.seq, dc.minor, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      return;
  }
}

// ---- 45: OpenFont ----
// Request body:
//   CARD32 fid
//   CARD16 nbytes
//   CARD16 pad
//   LISTofCHAR name (padded to 4)
void FontOps::handleOpenFont(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }

  const uint32_t fid = br.readU32();
  const uint16_t nbytes = br.readU16();
  br.skip(2);

  // Peek name (do not advance yet)
  std::string name;
  if (const uint8_t* p = br.peekBytes(nbytes)) {
    name.assign(reinterpret_cast<const char*>(p), nbytes);
  } else {
    name = "<truncated>";
  }

  // Consume name bytes + pad
  const size_t want = (size_t)nbytes;
  if (br.remaining() < want) { br.skip(br.remaining()); return; }
  br.skip(want);
  br.skip(br.remaining());

  if (fid == 0) return;

  // Wire to FontTable (server-owned)
  const bool ok = ctx.fonts().open(fid, name);

#ifndef NDEBUG
  ctx.tracef("[FontOps] OpenFont fid=0x%08X name=\"%s\" -> %s\n",
             (unsigned)fid, name.c_str(), ok ? "OK" : "FAIL");
#endif

  // If you want to be ultra-forgiving during bring-up, you can force OK by
  // mapping unknown names to fixed in FontTable::findByName (recommended),
  // so ok should almost always be true.
}
  
  
// MARK: ---- 46: CloseFont ----
// Request body: CARD32 fid
void FontOps::handleCloseFont(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t fid = br.readU32();
  br.skip(br.remaining());
  ctx.fonts().close(fid);
  
#ifndef NDEBUG
  ctx.tracef("[FontOps] CloseFont fid=0x%08X\n", (unsigned)fid);
#endif
}

// MARK: ---- 47: QueryFont ----
void FontOps::handleQueryFont(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t fid = br.readU32();
  br.skip(br.remaining());

  const x11::font::BdfFont* f = ctx.fonts().get(fid);
  if (!f) f = ctx.fonts().findByName("fixed");
  if (!f) return;

  if (!f->boundsValid) {
    const_cast<x11::font::BdfFont*>(f)->computeBounds();
  }

  auto toCI = [](const x11::font::CharInfo& ci) -> xCharInfo {
    xCharInfo o{};
    o.leftSideBearing  = (INT16)ci.lsb;
    o.rightSideBearing = (INT16)ci.rsb;
    o.characterWidth   = (INT16)ci.width;
    o.ascent           = (INT16)ci.ascent;
    o.descent          = (INT16)ci.descent;
    o.attributes       = (CARD16)ci.attr;
    return o;
  };

  // Build per-char table 0..255
  xCharInfo per[256];
  int fallbackEnc = f->defaultChar;
  if (!f->getGlyph(fallbackEnc)) fallbackEnc = (int)' ';
  if (!f->getGlyph(fallbackEnc)) fallbackEnc = (int)'?';

  xCharInfo fallbackCI = toCI(f->maxBounds);
  if (auto* g = f->getGlyph(fallbackEnc)) fallbackCI = toCI(g->charInfo());

  auto normalize = [](xCharInfo& ci) {
    if (ci.characterWidth == 0) ci.characterWidth = 8;
    if (ci.rightSideBearing == 0 && ci.leftSideBearing == 0)
      ci.rightSideBearing = ci.characterWidth;
    if (ci.ascent == 0 && ci.descent == 0) { ci.ascent = 11; ci.descent = 2; }
  };

  for (int code = 0; code < 256; ++code) {
    if (auto* g = f->getGlyph(code)) per[code] = toCI(g->charInfo());
    else per[code] = fallbackCI;
    normalize(per[code]);
  }

  // Build props
  struct LocalFontProp { uint32_t name; uint32_t value; };
  auto atom = [](const char* s) -> uint32_t {
    return x11::AtomTable::instance().intern(s, std::strlen(s), false);
  };

  std::vector<xFontProp> props;
  props.reserve(10);

  auto pushProp = [&](const char* name, uint32_t value) {
    xFontProp p{};
    p.name  = (CARD32)atom(name);
    p.value = (CARD32)value;
    props.push_back(p);
  };

  pushProp("FONT_ASCENT",  (uint32_t)f->ascent);
  pushProp("FONT_DESCENT", (uint32_t)f->descent);
  // SPACING is an ATOM value
  {
    xFontProp p{};
    p.name  = (CARD32)atom("SPACING");
    p.value = (CARD32)atom("C");
    props.push_back(p);
  }
  uint32_t avgW = (uint32_t)((f->maxBounds.width > 0) ? f->maxBounds.width : 6);
  pushProp("AVERAGE_WIDTH", avgW * 10u);
  uint32_t pt = (uint32_t)((f->pointSize > 0) ? f->pointSize : 13);
  pushProp("POINT_SIZE", pt * 10u);
  {
    xFontProp p{};
    p.name  = (CARD32)atom("CHARSET_REGISTRY");
    p.value = (CARD32)atom("ISO10646");
    props.push_back(p);
  }
  {
    xFontProp p{};
    p.name  = (CARD32)atom("CHARSET_ENCODING");
    p.value = (CARD32)atom("1");
    props.push_back(p);
  }

  // Fill the 60-byte reply struct exactly
  xQueryFontReply rep{};
  std::memset(&rep, 0, sizeof(rep));
  rep.type = 1;                 // X_Reply
  rep.sequenceNumber = seq;

  const xCharInfo ciMin = toCI(f->minBounds);
  rep.minBounds.leftSideBearing  = ciMin.leftSideBearing;
  rep.minBounds.rightSideBearing = ciMin.rightSideBearing;
  rep.minBounds.characterWidth  = ciMin.characterWidth;
  rep.minBounds.ascent           = ciMin.ascent;
  rep.minBounds.descent          = ciMin.descent;
  rep.minBounds.attributes       = ciMin.attributes;

  const xCharInfo ciMax = toCI(f->maxBounds);
  rep.maxBounds.leftSideBearing  = ciMax.leftSideBearing;
  rep.maxBounds.rightSideBearing = ciMax.rightSideBearing;
  rep.maxBounds.characterWidth  = ciMax.characterWidth;
  rep.maxBounds.ascent           = ciMax.ascent;
  rep.maxBounds.descent          = ciMax.descent;
  rep.maxBounds.attributes       = ciMax.attributes;
  rep.minCharOrByte2 = 0;
  rep.maxCharOrByte2 = 255;

  // Choose a defaultChar that exists
  int def = f->defaultChar;
  if (!f->getGlyph(def)) def = (int)' ';
  if (!f->getGlyph(def)) def = (int)'?';
  rep.defaultChar = (CARD16)def;

  rep.nFontProps = (CARD16)props.size();
  rep.drawDirection = 0;  // LeftToRight
  rep.minByte1 = 0;
  rep.maxByte1 = 0;
  rep.allCharsExist = 1;
  rep.fontAscent = (INT16)f->ascent;
  rep.fontDescent = (INT16)f->descent;
  rep.nCharInfos = 256;

// xxx old but seemed to work  // length is 4-byte units AFTER the base reply (60 bytes)
// xxx old but seemed to work  // variable bytes = props*8 + charInfos*12
// xxx old but seemed to work  const uint32_t varBytes = (uint32_t)(props.size() * sizeof(xFontProp)) + (uint32_t)sizeof(per);
// xxx old but seemed to work  rep.length = varBytes / 4u;

  const uint32_t fixedAfter32 = 28;
  const uint32_t propsBytes   = (uint32_t)(props.size() * sizeof(xFontProp)); //nFontProps * 8;
  const uint32_t infosBytes   = (uint32_t)sizeof(per); //nCharInfos * 12;
  rep.length = (fixedAfter32 + propsBytes + infosBytes) / 4;
  
  // Sanity: Xproto says this is 60 bytes
  static_assert(sizeof(xQueryFontReply) == 60, "xQueryFontReply must be 60 bytes");
  static_assert(sizeof(xFontProp) == 8, "xFontProp must be 8 bytes");
  static_assert(sizeof(xCharInfo) == 12, "xCharInfo must be 12 bytes");

  // Send reply struct (60 bytes), then variable lists
  // xxx old but seemed to work  if (!ctx.reply().sendReplyRaw(&rep, sizeof(rep))) return;
  // xxx old but seemed to work  if (!props.empty()) {
  // xxx old but seemed to work    if (!ctx.reply().sendReplyRaw(props.data(), props.size() * sizeof(xFontProp))) return;
  // xxx old but seemed to work  }
  // xxx old but seemed to work  (void)ctx.reply().sendReplyRaw(per, sizeof(per));
  
  // Send reply as X11 expects: 32-byte reply header first, then payload bytes.
  // xQueryFontReply is 60 bytes: [32-byte reply header][28 bytes fixed fields].
  const uint8_t* repBytes = reinterpret_cast<const uint8_t*>(&rep);

  // 1) 32-byte reply header
  if (!ctx.reply().sendReplyRaw(repBytes, 32)) return;

  // 2) remaining fixed fields (28 bytes)
  if (!ctx.reply().sendReplyRaw(repBytes + 32, sizeof(rep) - 32)) return;

  // 3) variable lists
  if (!props.empty()) {
    if (!ctx.reply().sendReplyRaw(props.data(), props.size() * sizeof(xFontProp))) return;
  }
  (void)ctx.reply().sendReplyRaw(per, sizeof(per));
}
  
// MARK: ---- 48: QueryTextExtents ----
// Request body:
//   CARD32 font (fid)
//   STRING16 chars (if oddLength=0) else STRING8?  (core protocol uses odd flag)
// Reply: fixed 32 bytes (xQueryTextExtentsReply)
void FontOps::handleQueryTextExtents(XProtoContext& ctx, uint16_t seq, bool /*oddLength*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t fid = br.readU32();
  (void)fid;

  // Consume remaining string (we ignore content; compute simple width)
  const size_t nbytes = br.remaining();
  br.skip(nbytes);

  // Very rough: assume 8px advance per byte (works well enough for bring-up)
  const int32_t overallWidth = (int32_t)nbytes * 8;

  // xQueryTextExtentsReply fields we care about:
  //  rep[1] = drawDirection
  //  rep[8..9]  = fontAscent
  //  rep[10..11]= fontDescent
  //  rep[12..13]= overallAscent
  //  rep[14..15]= overallDescent
  //  rep[16..17]= overallWidth
  //  rep[18..19]= overallLeft
  //  rep[20..21]= overallRight
  const uint16_t ascent = 12;
  const uint16_t descent = 4;

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = 0; // LeftToRight
    wire::wr16_le(rep.data() + 8, ascent);
    wire::wr16_le(rep.data() + 10, descent);
    wire::wr16_le(rep.data() + 12, ascent);
    wire::wr16_le(rep.data() + 14, descent);
    wire::wr16_le(rep.data() + 16, (uint16_t)overallWidth);
    wire::wr16_le(rep.data() + 18, 0);
    wire::wr16_le(rep.data() + 20, (uint16_t)overallWidth);
    // length=0 words (fixed reply)
    wire::wr32_le(rep.data() + 4, 0);
  });
}

// MARK: ---- 97: QueryBestSize ----
void FontOps::handleQueryBestSize(XProtoContext& ctx, uint16_t seq, uint8_t class_, ByteReader& br)
{
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }

  const uint32_t drawable = br.readU32();
  (void)drawable;
  const uint16_t w = br.readU16();
  const uint16_t h = br.readU16();
  br.skip(br.remaining());

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = class_;              // class echoed back
    wire::wr32_le(rep.data() + 4, 0); // length = 0
    wire::wr16_le(rep.data() + 8,  w);
    wire::wr16_le(rep.data() + 10, h);
  });
}
  
  
} // namespace x11
