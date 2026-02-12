//
//  BDF.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#include "Fonts/BDF.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <algorithm>

namespace x11::font {

static inline std::string trim(std::string s) {
  auto notSpace = [](unsigned char c){ return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

static inline bool startsWith(const std::string& s, const char* pfx) {
  return s.rfind(pfx, 0) == 0;
}

static inline bool parseInt(const std::string& s, int& out) {
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (!end || end == s.c_str()) return false;
  out = (int)v;
  return true;
}

// Parse a BITMAP hex line into bytes (rowStride bytes).
static bool parseHexRow(const std::string& hex, uint8_t* dst, int rowStride) {
  // Hex string length may be shorter; pad left with 0s.
  std::string h = trim(hex);
  if (h.empty()) {
    std::fill(dst, dst + rowStride, 0);
    return true;
  }
  // Ensure even length
  if (h.size() & 1u) h = "0" + h;

  // We want exactly rowStride bytes. BDF provides enough bytes to cover bbx_w rounded to 8.
  // We parse from left to right into bytes; if too few bytes, pad right with zeros.
  int bytesParsed = 0;
  for (size_t i = 0; i + 1 < h.size() && bytesParsed < rowStride; i += 2) {
    char buf[3] = { h[i], h[i+1], 0 };
    unsigned v = 0;
    if (std::sscanf(buf, "%x", &v) != 1) return false;
    dst[bytesParsed++] = (uint8_t)v;
  }
  while (bytesParsed < rowStride) dst[bytesParsed++] = 0;
  return true;
}

void BdfFont::computeBounds() {
  // Initialize min/max conservatively
  bool any = false;
  CharInfo minB{};
  CharInfo maxB{};

  for (const auto& kv : glyphs) {
    const Glyph& g = kv.second;
    CharInfo ci = g.charInfo();
    if (!any) {
      minB = maxB = ci;
      any = true;
      continue;
    }
    minB.lsb = std::min(minB.lsb, ci.lsb);
    minB.rsb = std::min(minB.rsb, ci.rsb);
    minB.width = std::min(minB.width, ci.width);
    minB.ascent = std::min(minB.ascent, ci.ascent);
    minB.descent = std::min(minB.descent, ci.descent);

    maxB.lsb = std::max(maxB.lsb, ci.lsb);
    maxB.rsb = std::max(maxB.rsb, ci.rsb);
    maxB.width = std::max(maxB.width, ci.width);
    maxB.ascent = std::max(maxB.ascent, ci.ascent);
    maxB.descent = std::max(maxB.descent, ci.descent);
  }

  if (!any) {
    // fallback: 8x16-ish
    minB = CharInfo{0, 8, 8, (int16_t)ascent, (int16_t)descent, 0};
    maxB = minB;
  }

  minBounds = minB;
  maxBounds = maxB;
  boundsValid = true;
}

BdfParseResult parseBdf(const std::string& text, BdfFont& out) {
  BdfParseResult r;
  out = BdfFont{};

  std::istringstream iss(text);
  std::string line;

  bool inProps = false;
  bool inChar = false;
  bool inBitmap = false;

  Glyph cur{};
  int bitmapRowsRead = 0;

  while (std::getline(iss, line)) {
    line = trim(line);
    if (line.empty()) continue;

    if (!inChar) {
      if (startsWith(line, "FONT ")) {
        out.name = trim(line.substr(5));
      } else if (startsWith(line, "SIZE ")) {
        // SIZE point xres yres
        std::istringstream ls(line.substr(5));
        ls >> out.pointSize;
      } else if (startsWith(line, "FONTBOUNDINGBOX ")) {
        std::istringstream ls(line.substr(16));
        ls >> out.bbx_w >> out.bbx_h >> out.bbx_xoff >> out.bbx_yoff;
      } else if (startsWith(line, "STARTPROPERTIES")) {
        inProps = true;
      } else if (inProps && startsWith(line, "FONT_ASCENT ")) {
        int v=0; if (parseInt(line.substr(11), v)) out.ascent = v;
      } else if (inProps && startsWith(line, "FONT_DESCENT ")) {
        int v=0; if (parseInt(line.substr(12), v)) out.descent = v;
      } else if (inProps && startsWith(line, "DEFAULT_CHAR ")) {
        int v=0; if (parseInt(line.substr(13), v)) out.defaultChar = v;
      } else if (inProps && startsWith(line, "ENDPROPERTIES")) {
        inProps = false;
      } else if (startsWith(line, "STARTCHAR")) {
        inChar = true;
        inBitmap = false;
        cur = Glyph{};
        bitmapRowsRead = 0;
      }
      continue;
    }

    // In a character block
    if (startsWith(line, "ENDCHAR")) {
      // finalize glyph
      if (cur.encoding >= 0) {
        out.glyphs[cur.encoding] = std::move(cur);
      }
      inChar = false;
      inBitmap = false;
      continue;
    }

    if (startsWith(line, "ENCODING ")) {
      int v=0; if (parseInt(line.substr(9), v)) cur.encoding = v;
      continue;
    }

    if (startsWith(line, "DWIDTH ")) {
      // DWIDTH x y
      std::istringstream ls(line.substr(7));
      int x=0, y=0;
      ls >> x >> y;
      cur.dwidth = x;
      continue;
    }

    if (startsWith(line, "BBX ")) {
      std::istringstream ls(line.substr(4));
      ls >> cur.bbx_w >> cur.bbx_h >> cur.bbx_xoff >> cur.bbx_yoff;
      continue;
    }

    if (startsWith(line, "BITMAP")) {
      inBitmap = true;
      bitmapRowsRead = 0;
      // allocate bitmap storage
      const int stride = cur.rowStrideBytes();
      if (cur.bbx_h < 0 || cur.bbx_h > 4096) {
        r.ok = false; r.error = "bad BBX height";
        return r;
      }
      cur.bitmap.assign((size_t)stride * (size_t)cur.bbx_h, 0);
      continue;
    }

    if (inBitmap) {
      const int stride = cur.rowStrideBytes();
      if (bitmapRowsRead < cur.bbx_h) {
        uint8_t* row = cur.bitmap.data() + (size_t)bitmapRowsRead * (size_t)stride;
        if (!parseHexRow(line, row, stride)) {
          r.ok = false; r.error = "bad BITMAP hex row";
          return r;
        }
        bitmapRowsRead++;
      }
      continue;
    }

    // ignore other lines (SWIDTH, ATTRIBUTES, etc.) for now
  }

  // Post-process bounds
  if (!out.boundsValid) out.computeBounds();
  r.ok = true;
  return r;
}

} // namespace x11::font
