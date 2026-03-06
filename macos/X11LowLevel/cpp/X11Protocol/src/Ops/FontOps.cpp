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
//#include "Core/XProtoTypes.hpp"
//#include "Core/XProtoWire.hpp"
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
#include "Utils/TraceDefs.hpp"

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
  reg.registerMajor(x11::opcode::ListFonts,         &FontOps::onMajor, this); // ListFonts
  reg.registerMajor(x11::opcode::ListFontsWithInfo,  &FontOps::onMajor, this); // ListFontsWithInfo
  reg.registerMajor(x11::opcode::QueryBestSize   , &FontOps::onMajor, this); // QueryBestSize
  reg.registerMajor(x11::opcode::SetFontPath     , &FontOps::onMajor, this); // 51
  reg.registerMajor(x11::opcode::GetFontPath     , &FontOps::onMajor, this); // 52
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
    case x11::opcode::ListFonts:         handleListFonts(ctx, dc.seq, dc.br); return;
    case x11::opcode::ListFontsWithInfo: handleListFontsWithInfo(ctx, dc.seq, dc.br); return;
    case x11::opcode::QueryBestSize   : handleQueryBestSize(ctx, dc.seq, dc.minor, dc.br); return;
    case x11::opcode::SetFontPath    : handleSetFontPath(ctx, dc.seq, dc.br); return;
    case x11::opcode::GetFontPath    : handleGetFontPath(ctx, dc.seq, dc.br); return;
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
  [[maybe_unused]] const bool ok = ctx.fonts().open(fid, name);

#if X11_TRACE_FONT_ENABLED
  fprintf(stderr, "[FontOps] OpenFont fid=0x%08X name=\"%s\" -> %s\n",
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
  void FontOps::handleQueryTextExtents(XProtoContext& ctx, uint16_t seq, bool oddLength, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t fid = br.readU32();

    // Resolve font (authoritative)
    const x11::font::BdfFont* f = ctx.fonts().get(fid);
    if (!f) f = ctx.fonts().findByName("fixed");
    if (!f) { br.skip(br.remaining()); return; }

    // X11 QueryTextExtents request:
    //  - oddLength==0: CHAR2B stream (2 bytes each)
    //  - oddLength==1: CHAR8 stream, padded to even total bytes after font id
    std::vector<uint16_t> codes;

    if (!oddLength) {
      // CHAR2B stream (byte1, byte2)
      const size_t n2 = br.remaining() / 2;
      codes.reserve(n2);
      for (size_t i = 0; i < n2; i++) {
        const uint8_t b1 = br.readU8();
        const uint8_t b2 = br.readU8();
        const uint16_t code = (uint16_t(b1) << 8) | uint16_t(b2);
        codes.push_back(code);
      }
      // Defensive: consume any trailing odd byte (should not happen)
      br.skip(br.remaining());
    } else {
      // CHAR8 stream; if remaining is odd, last byte is pad to 2-byte boundary.
      size_t n1 = br.remaining();
      const bool hasPad = (n1 & 1u) != 0;
      if (hasPad) n1 -= 1;

      codes.reserve(n1);
      for (size_t i = 0; i < n1; i++) {
        const uint8_t b = br.readU8();
        codes.push_back(uint16_t(b));
      }

      if (hasPad && br.remaining() > 0) {
        (void)br.readU8(); // consume pad
      }
      br.skip(br.remaining());
    }

    // Compute extents
    int32_t overallWidth = 0;
    int32_t overallAscent = 0;
    int32_t overallDescent = 0;
    int32_t overallLeft = 0;
    int32_t overallRight = 0;

    bool firstGlyph = true;
    int32_t pen = 0;

    auto glyphCharInfo = [&](uint16_t code) -> x11::font::CharInfo {
      // Support 0..255 only (Latin-1); fallback otherwise.
      int enc = (code <= 255u) ? int(code) : f->defaultChar;
      if (enc < 0) enc = f->defaultChar;

      const x11::font::Glyph* g = f->getGlyph(enc);
      if (!g) g = f->getGlyph(f->defaultChar);
      if (!g) g = f->getGlyph(int('?'));
      if (g) return g->charInfo();

      // Worst-case fallback
      x11::font::CharInfo ci{};
      const int w = (f->bbx_w > 0) ? f->bbx_w : 8;
      ci.lsb = 0;
      ci.rsb = (int16_t)w;
      ci.width = (int16_t)w;
      ci.ascent = (int16_t)((f->ascent > 0) ? f->ascent : 12);
      ci.descent = (int16_t)((f->descent > 0) ? f->descent : 4);
      ci.attr = 0;
      return ci;
    };

    for (uint16_t code : codes) {
      const auto ci = glyphCharInfo(code);

      const int32_t left  = pen + ci.lsb;
      const int32_t right = pen + ci.rsb;

      if (firstGlyph) {
        overallLeft = left;
        overallRight = right;
        firstGlyph = false;
      } else {
        if (left < overallLeft) overallLeft = left;
        if (right > overallRight) overallRight = right;
      }

      if (ci.ascent > overallAscent) overallAscent = ci.ascent;
      if (ci.descent > overallDescent) overallDescent = ci.descent;

      const int adv = (ci.width != 0) ? ci.width : f->advanceFor(int(code & 0xFFu));
      pen += adv;
    }

    overallWidth = pen;

    const uint16_t fontAscent  = (uint16_t)((f->ascent  > 0) ? f->ascent  : overallAscent);
    const uint16_t fontDescent = (uint16_t)((f->descent > 0) ? f->descent : overallDescent);

    auto clamp16 = [](int32_t v) -> uint16_t {
      if (v < -32768) v = -32768;
      if (v >  32767) v =  32767;
      return (uint16_t)(int16_t)v;
    };

  #ifndef NDEBUG
    // Summary log: helps debug xterm geometry/columns decisions.
    ctx.tracef("[FontOps] QueryTextExtents seq=%u fid=0x%08X odd=%u nchars=%zu "
               "font=\"%s\" bbx=%dx%d ascent=%d descent=%d "
               "W=%d L=%d R=%d A=%d D=%d\n",
               (unsigned)seq,
               (unsigned)fid,
               (unsigned)(oddLength ? 1 : 0),
               codes.size(),
               f->name.c_str(),
               f->bbx_w, f->bbx_h,
               f->ascent, f->descent,
               (int)overallWidth,
               (int)overallLeft,
               (int)overallRight,
               (int)overallAscent,
               (int)overallDescent);
  #endif

    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = 0; // drawDirection: LeftToRight
      wire::wr32_le(rep.data() + 4, 0); // length=0 (fixed reply)

      wire::wr16_le(rep.data() + 8,  fontAscent);
      wire::wr16_le(rep.data() + 10, fontDescent);

      wire::wr16_le(rep.data() + 12, clamp16(overallAscent));
      wire::wr16_le(rep.data() + 14, clamp16(overallDescent));
      wire::wr16_le(rep.data() + 16, clamp16(overallWidth));
      wire::wr16_le(rep.data() + 18, clamp16(overallLeft));
      wire::wr16_le(rep.data() + 20, clamp16(overallRight));
    });
  }  

// MARK: ---- 49: ListFonts ----
void FontOps::handleListFonts(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }

  const uint16_t maxNames   = br.readU16();
  const uint16_t patternLen = br.readU16();

  std::string pattern;
  if (patternLen > 0 && br.remaining() >= patternLen) {
    if (const uint8_t* p = br.peekBytes(patternLen)) {
      pattern.assign(reinterpret_cast<const char*>(p), patternLen);
    }
  }
  br.skip(br.remaining());

  // Get all known font names (short names + XLFD names + aliases)
  std::vector<std::string> allNames = ctx.fonts().listNames();

  // Lowercase the pattern for case-insensitive matching
  std::string lpat = pattern;
  for (char& c : lpat) c = (char)std::tolower((unsigned char)c);

  // Full glob matching: supports * (any substring) and ? (single char)
  // in any position, enabling proper XLFD wildcard patterns like
  // "-*-fixed-*-*-*-*-*-*-*-*-*-*-*-*" or "-*-helvetica-bold-*"
  auto matches = [&](const std::string& name) -> bool {
    if (lpat.empty() || lpat == "*") return true;
    return fontGlobMatch(lpat.c_str(), name.c_str());
  };

  std::vector<std::string> matched;
  for (const auto& n : allNames) {
    if (matches(n)) {
      matched.push_back(n);
      if (matched.size() >= (size_t)maxNames) break;
    }
  }

  // Build payload: list of STR8 (1 byte length + string bytes, no inter-item padding)
  std::vector<uint8_t> payload;
  for (const auto& n : matched) {
    uint8_t len = (uint8_t)std::min(n.size(), (size_t)255);
    payload.push_back(len);
    payload.insert(payload.end(), n.begin(), n.begin() + len);
  }

  const uint16_t nNames = (uint16_t)matched.size();
  const uint32_t payloadPadded = (uint32_t)((payload.size() + 3u) & ~3u);
  const uint32_t lengthWords = payloadPadded / 4u;

#ifndef NDEBUG
  ctx.tracef("[FontOps] ListFonts seq=%u maxNames=%u pattern=\"%s\" -> %u names\n",
             (unsigned)seq, (unsigned)maxNames, pattern.c_str(), (unsigned)nNames);
#endif

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = 0; // unused
    wire::wr32_le(rep.data() + 4, lengthWords);
    wire::wr16_le(rep.data() + 8, nNames);
  });

  if (!payload.empty()) {
    (void)ctx.reply().sendPaddedBytes(payload.data(), payload.size());
  }
}

// MARK: ---- 50: ListFontsWithInfo ----
//
// Reply format (xListFontsWithInfoReply, 60 bytes):
//   type=1, nameLength=len, seq, length
//   minBounds (xCharInfo 12B) + pad4
//   maxBounds (xCharInfo 12B) + pad4
//   minCharOrByte2, maxCharOrByte2, defaultChar, nFontProps
//   drawDirection, minByte1, maxByte1, allCharsExist
//   fontAscent, fontDescent, nReplies(hint)
//   Then: nFontProps * xFontProp (8B each) + name (nameLength bytes, padded to 4)
// Terminator: nameLength=0, length=7
//
void FontOps::handleListFontsWithInfo(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }

  const uint16_t maxNames   = br.readU16();
  const uint16_t patternLen = br.readU16();

  std::string pattern;
  if (patternLen > 0 && br.remaining() >= patternLen) {
    if (const uint8_t* p = br.peekBytes(patternLen)) {
      pattern.assign(reinterpret_cast<const char*>(p), patternLen);
    }
  }
  br.skip(br.remaining());

  // Lowercase pattern for matching
  std::string lpat = pattern;
  for (char& c : lpat) c = (char)std::tolower((unsigned char)c);

  // Collect matching fonts
  std::vector<std::string> allNames = ctx.fonts().listNames();

  // Deduplicate: for each matching name, resolve to font pointer and
  // track which fonts we've already sent (avoids sending the same font
  // twice when it appears as both a short name and an XLFD name).
  std::unordered_set<const x11::font::BdfFont*> sent;
  uint32_t count = 0;

  // First pass: count matches for the nReplies hint
  uint32_t totalMatches = 0;
  for (const auto& n : allNames) {
    if (lpat.empty() || lpat == "*" || fontGlobMatch(lpat.c_str(), n.c_str())) {
      totalMatches++;
    }
  }

#ifndef NDEBUG
  ctx.tracef("[FontOps] ListFontsWithInfo seq=%u maxNames=%u pattern=\"%s\" totalMatches=%u\n",
             (unsigned)seq, (unsigned)maxNames, pattern.c_str(), (unsigned)totalMatches);
#endif

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

  for (const auto& n : allNames) {
    if (count >= maxNames) break;

    if (!lpat.empty() && lpat != "*" && !fontGlobMatch(lpat.c_str(), n.c_str()))
      continue;

    const x11::font::BdfFont* f = ctx.fonts().findByName(n);
    if (!f) continue;

    // Deduplicate by font pointer
    if (sent.count(f)) continue;
    sent.insert(f);

    if (!f->boundsValid) {
      const_cast<x11::font::BdfFont*>(f)->computeBounds();
    }

    // Build properties (same as QueryFont)
    auto atom = [](const char* s) -> uint32_t {
      return x11::AtomTable::instance().intern(s, std::strlen(s), false);
    };

    std::vector<xFontProp> props;
    props.reserve(7);
    auto pushProp = [&](const char* name, uint32_t value) {
      xFontProp p{};
      p.name  = (CARD32)atom(name);
      p.value = (CARD32)value;
      props.push_back(p);
    };

    pushProp("FONT_ASCENT",  (uint32_t)f->ascent);
    pushProp("FONT_DESCENT", (uint32_t)f->descent);
    { xFontProp p{}; p.name = (CARD32)atom("SPACING"); p.value = (CARD32)atom("C"); props.push_back(p); }
    uint32_t avgW = (uint32_t)((f->maxBounds.width > 0) ? f->maxBounds.width : 6);
    pushProp("AVERAGE_WIDTH", avgW * 10u);
    uint32_t pt = (uint32_t)((f->pointSize > 0) ? f->pointSize : 13);
    pushProp("POINT_SIZE", pt * 10u);
    { xFontProp p{}; p.name = (CARD32)atom("CHARSET_REGISTRY"); p.value = (CARD32)atom("ISO10646"); props.push_back(p); }
    { xFontProp p{}; p.name = (CARD32)atom("CHARSET_ENCODING"); p.value = (CARD32)atom("1"); props.push_back(p); }

    // Use the XLFD name from the font as the reply name
    const std::string& fontName = f->name.empty() ? n : f->name;
    uint8_t nameLen = (uint8_t)std::min(fontName.size(), (size_t)255);

    // Calculate length: 28 bytes fixed (after 32-byte header) + props + name padded
    uint32_t propsBytes = (uint32_t)(props.size() * sizeof(xFontProp));
    uint32_t namePadded = ((uint32_t)nameLen + 3u) & ~3u;
    uint32_t lengthWords = (28u + propsBytes + namePadded) / 4u;

    uint32_t repliesHint = totalMatches - count - 1; // approximate remaining

    // Build 60-byte reply header
    // Using xListFontsWithInfoReply which has the same layout as xQueryFontReply
    // but with nameLength in byte 1 and nReplies at offset 56
    xQueryFontReply rep{};
    std::memset(&rep, 0, sizeof(rep));
    rep.type = 1; // X_Reply
    // rep.pad1 is at byte 1 — we'll overwrite it with nameLength below
    rep.sequenceNumber = seq;
    rep.length = lengthWords;

    const xCharInfo ciMin = toCI(f->minBounds);
    rep.minBounds.leftSideBearing  = ciMin.leftSideBearing;
    rep.minBounds.rightSideBearing = ciMin.rightSideBearing;
    rep.minBounds.characterWidth   = ciMin.characterWidth;
    rep.minBounds.ascent           = ciMin.ascent;
    rep.minBounds.descent          = ciMin.descent;
    rep.minBounds.attributes       = ciMin.attributes;

    const xCharInfo ciMax = toCI(f->maxBounds);
    rep.maxBounds.leftSideBearing  = ciMax.leftSideBearing;
    rep.maxBounds.rightSideBearing = ciMax.rightSideBearing;
    rep.maxBounds.characterWidth   = ciMax.characterWidth;
    rep.maxBounds.ascent           = ciMax.ascent;
    rep.maxBounds.descent          = ciMax.descent;
    rep.maxBounds.attributes       = ciMax.attributes;

    rep.minCharOrByte2 = 0;
    rep.maxCharOrByte2 = 255;

    int def = f->defaultChar;
    if (!f->getGlyph(def)) def = (int)' ';
    if (!f->getGlyph(def)) def = (int)'?';
    rep.defaultChar = (CARD16)def;

    rep.nFontProps = (CARD16)props.size();
    rep.drawDirection = 0; // LeftToRight
    rep.minByte1 = 0;
    rep.maxByte1 = 0;
    rep.allCharsExist = 1;
    rep.fontAscent = (INT16)f->ascent;
    rep.fontDescent = (INT16)f->descent;
    // nCharInfos field is at same offset as nReplies in ListFontsWithInfo
    rep.nCharInfos = (CARD32)repliesHint;

    // Patch nameLength into byte 1 (pad1 in xQueryFontReply)
    uint8_t* repBytes = reinterpret_cast<uint8_t*>(&rep);
    repBytes[1] = nameLen;

    // Send the 60-byte reply header
    if (!ctx.reply().sendReplyRaw(repBytes, 32)) break;
    if (!ctx.reply().sendReplyRaw(repBytes + 32, sizeof(rep) - 32)) break;

    // Send properties
    if (!props.empty()) {
      if (!ctx.reply().sendReplyRaw(props.data(), props.size() * sizeof(xFontProp))) break;
    }

    // Send font name (padded to 4 bytes)
    if (nameLen > 0) {
      uint8_t nameBuf[256 + 3] = {};
      std::memcpy(nameBuf, fontName.data(), nameLen);
      if (!ctx.reply().sendReplyRaw(nameBuf, namePadded)) break;
    }

    count++;
  }

  // Send terminator reply: nameLen=0, total 60 bytes (32-byte header + 28 bytes fixed fields)
  // The length=7 field tells the client there are 28 bytes after the 32-byte header.
  // We MUST send all 60 bytes or the client will hang waiting for the remaining data.
  {
    uint8_t term[60] = {};
    term[0] = 1;  // type = X_Reply
    term[1] = 0;  // nameLen = 0 (terminator)
    wire::wr16_le(term + 2, seq);
    wire::wr32_le(term + 4, 7); // length = 7 (28 bytes after 32-byte header)
    // All remaining 52 bytes are zero (already initialized)
    if (!ctx.reply().sendReplyRaw(term, 32)) return;
    (void)ctx.reply().sendReplyRaw(term + 32, 28);
  }
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
  
  
// MARK: ---- 51: SetFontPath (void, accept and ignore) ----
void FontOps::handleSetFontPath(XProtoContext& /*ctx*/, uint16_t /*seq*/, ByteReader& br) {
  br.skip(br.remaining());
}

// MARK: ---- 52: GetFontPath (stub: empty list) ----
void FontOps::handleGetFontPath(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  br.skip(br.remaining());
  (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
    // rep[8..9] = nStrings = 0 (already zeroed)
  });
}

} // namespace x11
