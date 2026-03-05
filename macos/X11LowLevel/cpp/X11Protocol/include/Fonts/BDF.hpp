//
//  BDF.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace x11::font {

struct CharInfo {
  int16_t  lsb = 0;   // left side bearing
  int16_t  rsb = 0;   // right side bearing
  int16_t  width = 0; // characterWidth (advance)
  int16_t  ascent = 0;
  int16_t  descent = 0;
  uint16_t attr = 0;
};

struct Glyph {
  int encoding = -1;      // BDF ENCODING
  int bbx_w = 0;
  int bbx_h = 0;
  int bbx_xoff = 0;
  int bbx_yoff = 0;
  int dwidth = 0;         // DWIDTH x
  std::vector<uint8_t> bitmap; // 1bpp packed rows, rowStrideBytes = ceil(bbx_w/8)
  std::vector<uint8_t> alpha;  // 8-bit grayscale coverage (bbx_w * bbx_h), empty for 1bpp-only

  bool hasAlpha() const { return !alpha.empty(); }

  CharInfo charInfo() const {
    CharInfo ci;
    ci.lsb = (int16_t)bbx_xoff;
    ci.rsb = (int16_t)(bbx_xoff + bbx_w);
    ci.width = (int16_t)dwidth;
    // ascent/descent relative to baseline using BBX yoff:
    // BBX yoff is the offset from baseline to bottom of glyph bitmap.
    // Bitmap spans [yoff .. yoff+bbx_h). Ascent is above baseline.
    const int top = bbx_yoff + bbx_h;
    const int bottom = bbx_yoff;
    ci.ascent = (int16_t)((top > 0) ? top : 0);
    ci.descent = (int16_t)((bottom < 0) ? -bottom : 0);
    ci.attr = 0;
    return ci;
  }

  int rowStrideBytes() const { return (bbx_w + 7) / 8; }
};

struct BdfFont {
  std::string name;
  int pointSize = 0;

  int bbx_w = 0, bbx_h = 0, bbx_xoff = 0, bbx_yoff = 0; // FONTBOUNDINGBOX
  int ascent = 0;
  int descent = 0;
  int defaultChar = 0;

  // encoding -> glyph
  std::unordered_map<int, Glyph> glyphs;

  // cached min/max bounds for QueryFont
  CharInfo minBounds{};
  CharInfo maxBounds{};
  bool boundsValid = false;

  const Glyph* getGlyph(int encoding) const {
    auto it = glyphs.find(encoding);
    return (it == glyphs.end()) ? nullptr : &it->second;
  }

  // For text extents: return advance width for encoding (fallback to bbx_w)
  int advanceFor(int encoding) const {
    if (const Glyph* g = getGlyph(encoding)) return g->dwidth ? g->dwidth : g->bbx_w;
    return (bbx_w > 0) ? bbx_w : 8;
  }

  void computeBounds();
};

struct BdfParseResult {
  bool ok = false;
  std::string error;
};

BdfParseResult parseBdf(const std::string& text, BdfFont& out);

} // namespace x11::font
