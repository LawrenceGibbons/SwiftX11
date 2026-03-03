//
//  PCF.cpp
//  X11LowLevel
//
//  PCF (Portable Compiled Font) parser.
//
//  PCF file format:
//    Header: 4-byte magic "\1fcp" (0x70636601 LE), then CARD32 tableCount
//    TOC: tableCount * { CARD32 type, CARD32 format, CARD32 size, CARD32 offset }
//    Tables: PROPERTIES, METRICS, BITMAPS, BDF_ENCODINGS, etc.
//
//  Each table starts with a 4-byte format word specifying byte order and
//  padding details. We handle both LE and BE byte orders.
//

#include "Fonts/PCF.hpp"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <zlib.h>

namespace x11::font {

// PCF constants
static constexpr uint32_t PCF_FILE_VERSION   = 0x70636601; // "\1fcp" in LE
static constexpr uint32_t PCF_PROPERTIES     = (1 << 0);
static constexpr uint32_t PCF_ACCELERATORS   = (1 << 1);
static constexpr uint32_t PCF_METRICS        = (1 << 2);
static constexpr uint32_t PCF_BITMAPS        = (1 << 3);
static constexpr uint32_t PCF_INK_METRICS    = (1 << 4);
static constexpr uint32_t PCF_BDF_ENCODINGS  = (1 << 5);
static constexpr uint32_t PCF_SWIDTHS        = (1 << 6);
static constexpr uint32_t PCF_GLYPH_NAMES    = (1 << 7);
static constexpr uint32_t PCF_BDF_ACCELERATORS = (1 << 8);

// Format bits
static constexpr uint32_t PCF_DEFAULT_FORMAT     = 0x00000000;
static constexpr uint32_t PCF_INKBOUNDS          = 0x00000200;
static constexpr uint32_t PCF_ACCEL_W_INKBOUNDS  = 0x00000100;
static constexpr uint32_t PCF_COMPRESSED_METRICS = 0x00000100;
static constexpr uint32_t PCF_BYTE_MASK          = (1 << 2); // 0=LE, 1=BE
static constexpr uint32_t PCF_BIT_MASK           = (1 << 3);
static constexpr uint32_t PCF_GLYPH_PAD_MASK     = (3 << 0); // 0=1byte, 1=2byte, 2=4byte

struct TocEntry {
  uint32_t type;
  uint32_t format;
  uint32_t size;
  uint32_t offset;
};

// ──────────────────────────────────────────────────────────────────────
// Byte-order-aware readers
// ──────────────────────────────────────────────────────────────────────

static inline uint16_t rd16(const uint8_t* p, bool be) {
  return be ? (uint16_t(p[0]) << 8 | p[1])
            : (uint16_t(p[1]) << 8 | p[0]);
}

static inline int16_t rd16s(const uint8_t* p, bool be) {
  return (int16_t)rd16(p, be);
}

static inline uint32_t rd32(const uint8_t* p, bool be) {
  return be ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3])
            : (uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]);
}

static inline int32_t rd32s(const uint8_t* p, bool be) {
  return (int32_t)rd32(p, be);
}

// TOC entries are always stored in LE (the file header magic is LE)
static inline uint32_t rd32le(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}


// ──────────────────────────────────────────────────────────────────────
// parsePcf — main PCF parser
// ──────────────────────────────────────────────────────────────────────

BdfParseResult parsePcf(const uint8_t* data, size_t len, BdfFont& out) {
  BdfParseResult result;

  if (len < 8) { result.error = "too short"; return result; }

  uint32_t magic = rd32le(data);
  if (magic != PCF_FILE_VERSION) {
    result.error = "bad PCF magic";
    return result;
  }

  uint32_t tableCount = rd32le(data + 4);
  if (tableCount > 1000) { result.error = "too many tables"; return result; }
  if (8 + tableCount * 16 > len) { result.error = "TOC truncated"; return result; }

  // Parse TOC
  std::vector<TocEntry> toc(tableCount);
  for (uint32_t i = 0; i < tableCount; i++) {
    const uint8_t* e = data + 8 + i * 16;
    toc[i].type   = rd32le(e + 0);
    toc[i].format = rd32le(e + 4);
    toc[i].size   = rd32le(e + 8);
    toc[i].offset = rd32le(e + 12);
  }

  auto findTable = [&](uint32_t type) -> const TocEntry* {
    for (const auto& t : toc) {
      if (t.type == type) return &t;
    }
    return nullptr;
  };

  // ---- Parse PROPERTIES table ----
  // This gives us the XLFD name, FONT_ASCENT, FONT_DESCENT, etc.
  if (const TocEntry* t = findTable(PCF_PROPERTIES)) {
    if (t->offset + 8 <= len) {
      const uint8_t* base = data + t->offset;
      uint32_t fmt = rd32le(base);
      bool be = (fmt & PCF_BYTE_MASK) != 0;
      base += 4;

      uint32_t nProps = rd32(base, be); base += 4;
      if (t->offset + 8 + nProps * 9 <= len) {
        // Property entries: 9 bytes each (name_offset: 4, isString: 1, value: 4)
        struct PropEntry { uint32_t nameOff; bool isString; uint32_t value; };
        std::vector<PropEntry> entries(nProps);
        for (uint32_t i = 0; i < nProps; i++) {
          entries[i].nameOff = rd32(base, be); base += 4;
          entries[i].isString = (base[0] != 0); base += 1;
          entries[i].value = rd32(base, be); base += 4;
        }
        // Pad to 4-byte boundary after property entries
        size_t propBytesUsed = 4 + 4 + nProps * 9;
        size_t padded = (propBytesUsed + 3) & ~3u;
        base = data + t->offset + padded;

        // String table
        if (base + 4 <= data + len) {
          uint32_t stringSize = rd32(base, be); base += 4;
          const char* strings = reinterpret_cast<const char*>(base);
          size_t stringsAvail = (data + len) - base;
          if (stringSize > stringsAvail) stringSize = (uint32_t)stringsAvail;

          auto getString = [&](uint32_t off) -> std::string {
            if (off >= stringSize) return "";
            size_t maxLen = stringSize - off;
            return std::string(strings + off, strnlen(strings + off, maxLen));
          };

          for (uint32_t i = 0; i < nProps; i++) {
            std::string propName = getString(entries[i].nameOff);
            if (entries[i].isString) {
              std::string propValue = getString(entries[i].value);
              if (propName == "FONT") out.name = propValue;
            } else {
              int32_t intVal = (int32_t)entries[i].value;
              if (propName == "FONT_ASCENT") out.ascent = intVal;
              else if (propName == "FONT_DESCENT") out.descent = intVal;
              else if (propName == "DEFAULT_CHAR") out.defaultChar = intVal;
              else if (propName == "POINT_SIZE") out.pointSize = intVal / 10; // decipoints
            }
          }
        }
      }
    }
  }

  // ---- Parse BDF_ACCELERATORS or ACCELERATORS for FONTBOUNDINGBOX ----
  for (uint32_t accType : {PCF_BDF_ACCELERATORS, PCF_ACCELERATORS}) {
    if (const TocEntry* t = findTable(accType)) {
      if (t->offset + 24 <= len) {
        const uint8_t* base = data + t->offset;
        uint32_t fmt = rd32le(base);
        bool be = (fmt & PCF_BYTE_MASK) != 0;
        base += 4;

        // Accelerator table layout:
        //   noOverlap(1), constantMetrics(1), terminalFont(1), constantWidth(1),
        //   inkInside(1), inkMetrics(1), drawDirection(1), padding(1),
        //   fontAscent(4), fontDescent(4),
        //   maxOverlap(4),
        //   minBounds (xCharInfo, 12 bytes),
        //   maxBounds (xCharInfo, 12 bytes)
        //   [if ACCEL_W_INKBOUNDS: ink_minBounds, ink_maxBounds]

        if (out.ascent == 0) out.ascent = rd32s(base + 8, be);
        if (out.descent == 0) out.descent = rd32s(base + 12, be);

        // maxBounds starts at offset 4 + 8 + 4 + 12 = 28
        const uint8_t* maxB = base + 8 + 4 + 4 + 12;
        if (maxB + 12 <= data + len) {
          int16_t mw = rd16s(maxB + 4, be); // characterWidth
          int16_t ma = rd16s(maxB + 6, be); // ascent
          int16_t md = rd16s(maxB + 8, be); // descent
          if (out.bbx_w == 0) out.bbx_w = mw;
          if (out.bbx_h == 0) out.bbx_h = ma + md;
        }

        break; // found accelerators, don't need both
      }
    }
  }

  // ---- Parse METRICS table ----
  struct PcfMetric {
    int16_t lsb, rsb, width, ascent, descent;
    uint16_t attr;
  };
  std::vector<PcfMetric> metrics;

  if (const TocEntry* t = findTable(PCF_METRICS)) {
    if (t->offset + 4 <= len) {
      const uint8_t* base = data + t->offset;
      uint32_t fmt = rd32le(base);
      bool be = (fmt & PCF_BYTE_MASK) != 0;
      bool compressed = (fmt & PCF_COMPRESSED_METRICS) != 0;
      base += 4;

      if (compressed) {
        if (base + 2 <= data + len) {
          uint16_t count = rd16(base, be); base += 2;
          metrics.resize(count);
          for (uint16_t i = 0; i < count && base + 5 <= data + len; i++) {
            // Compressed metrics: 5 bytes each, values biased by 0x80
            metrics[i].lsb    = (int16_t)base[0] - 0x80;
            metrics[i].rsb    = (int16_t)base[1] - 0x80;
            metrics[i].width  = (int16_t)base[2] - 0x80;
            metrics[i].ascent = (int16_t)base[3] - 0x80;
            metrics[i].descent= (int16_t)base[4] - 0x80;
            metrics[i].attr   = 0;
            base += 5;
          }
        }
      } else {
        if (base + 4 <= data + len) {
          uint32_t count = rd32(base, be); base += 4;
          metrics.resize(count);
          for (uint32_t i = 0; i < count && base + 12 <= data + len; i++) {
            metrics[i].lsb    = rd16s(base + 0, be);
            metrics[i].rsb    = rd16s(base + 2, be);
            metrics[i].width  = rd16s(base + 4, be);
            metrics[i].ascent = rd16s(base + 6, be);
            metrics[i].descent= rd16s(base + 8, be);
            metrics[i].attr   = rd16(base + 10, be);
            base += 12;
          }
        }
      }
    }
  }

  // ---- Parse BITMAPS table ----
  std::vector<uint32_t> bitmapOffsets;
  const uint8_t* bitmapData = nullptr;
  size_t bitmapDataSize = 0;
  int glyphPad = 0; // row padding in bytes: 1, 2, or 4

  if (const TocEntry* t = findTable(PCF_BITMAPS)) {
    if (t->offset + 4 <= len) {
      const uint8_t* base = data + t->offset;
      uint32_t fmt = rd32le(base);
      bool be = (fmt & PCF_BYTE_MASK) != 0;
      glyphPad = 1 << (fmt & PCF_GLYPH_PAD_MASK); // 1, 2, or 4
      bool msbFirst = (fmt & PCF_BIT_MASK) != 0;
      base += 4;

      if (base + 4 <= data + len) {
        uint32_t glyphCount = rd32(base, be); base += 4;

        // Glyph offsets
        if (base + glyphCount * 4 <= data + len) {
          bitmapOffsets.resize(glyphCount);
          for (uint32_t i = 0; i < glyphCount; i++) {
            bitmapOffsets[i] = rd32(base, be);
            base += 4;
          }
        }

        // 4 bitmap sizes (for 4 padding modes)
        if (base + 16 <= data + len) {
          base += 16; // skip bitmapSizes[4]
        }

        // Bitmap data starts here
        bitmapData = base;
        bitmapDataSize = (data + len) - base;

        // If bits are MSB first (which is standard for PCF), we may need
        // to bit-reverse each byte for our 1bpp format.
        // Our BDF parser stores glyphs with MSB-first bit order, so
        // PCF MSB-first is already correct for us.
        (void)msbFirst; // Our bitmap storage is MSB-first compatible
      }
    }
  }

  // ---- Parse BDF_ENCODINGS table ----
  // Maps character encoding (row, col) to glyph index
  std::vector<int32_t> encodingToGlyph; // enc -> glyph index, -1 = no glyph
  int minByte2 = 0, maxByte2 = 255;
  int minByte1 = 0, maxByte1 = 0;

  if (const TocEntry* t = findTable(PCF_BDF_ENCODINGS)) {
    if (t->offset + 14 <= len) {
      const uint8_t* base = data + t->offset;
      uint32_t fmt = rd32le(base);
      bool be = (fmt & PCF_BYTE_MASK) != 0;
      base += 4;

      minByte2 = rd16s(base + 0, be);
      maxByte2 = rd16s(base + 2, be);
      minByte1 = rd16s(base + 4, be);
      maxByte1 = rd16s(base + 6, be);
      /* defaultChar skipped: */ base += 10;

      int nRows = maxByte1 - minByte1 + 1;
      int nCols = maxByte2 - minByte2 + 1;
      int nEncodings = nRows * nCols;
      if (nEncodings > 0 && nEncodings < 65536 && base + nEncodings * 2 <= data + len) {
        encodingToGlyph.resize(nEncodings);
        for (int i = 0; i < nEncodings; i++) {
          uint16_t idx = rd16(base, be);
          encodingToGlyph[i] = (idx == 0xFFFF) ? -1 : (int32_t)idx;
          base += 2;
        }
      }
    }
  }

  // ---- Build glyphs ----
  if (metrics.empty()) {
    result.error = "no metrics table";
    return result;
  }

  uint32_t nGlyphs = (uint32_t)metrics.size();

  // Helper to get glyph index for an encoding
  auto encToGlyphIdx = [&](int enc) -> int {
    int row = enc >> 8;
    int col = enc & 0xFF;
    if (row < minByte1 || row > maxByte1) return -1;
    if (col < minByte2 || col > maxByte2) return -1;
    int idx = (row - minByte1) * (maxByte2 - minByte2 + 1) + (col - minByte2);
    if (idx < 0 || idx >= (int)encodingToGlyph.size()) return -1;
    return encodingToGlyph[idx];
  };

  // Iterate over encodings and create glyphs
  int maxEnc = (maxByte1 << 8) | maxByte2;
  int minEnc = (minByte1 << 8) | minByte2;

  for (int enc = minEnc; enc <= maxEnc && enc < 65536; enc++) {
    int gi = encToGlyphIdx(enc);
    if (gi < 0 || gi >= (int)nGlyphs) continue;

    const auto& m = metrics[gi];

    Glyph g;
    g.encoding = enc;
    g.dwidth = m.width;

    // Compute BBX from metrics
    int bbxW = m.rsb - m.lsb;
    int bbxH = m.ascent + m.descent;
    if (bbxW <= 0) bbxW = (m.width > 0) ? m.width : 1;
    if (bbxH <= 0) bbxH = 1;
    g.bbx_w = bbxW;
    g.bbx_h = bbxH;
    g.bbx_xoff = m.lsb;
    g.bbx_yoff = -m.descent;

    // Extract bitmap
    if (gi < (int)bitmapOffsets.size() && bitmapData) {
      uint32_t off = bitmapOffsets[gi];
      int srcStride = ((bbxW + 8 * glyphPad - 1) / (8 * glyphPad)) * glyphPad;
      int dstStride = (bbxW + 7) / 8;
      int totalSrcBytes = srcStride * bbxH;

      if (off + totalSrcBytes <= bitmapDataSize) {
        g.bitmap.resize(dstStride * bbxH, 0);
        for (int row = 0; row < bbxH; row++) {
          const uint8_t* srcRow = bitmapData + off + row * srcStride;
          uint8_t* dstRow = g.bitmap.data() + row * dstStride;
          std::memcpy(dstRow, srcRow, std::min(srcStride, dstStride));
        }
      }
    }

    out.glyphs[enc] = std::move(g);
  }

  // Set FONTBOUNDINGBOX from metrics if not already set from accelerators
  if (out.bbx_w <= 0 || out.bbx_h <= 0) {
    // Compute from metrics
    int maxW = 0, maxH = 0;
    for (const auto& m : metrics) {
      int w = m.rsb - m.lsb;
      int h = m.ascent + m.descent;
      if (w > maxW) maxW = w;
      if (h > maxH) maxH = h;
    }
    if (out.bbx_w <= 0) out.bbx_w = maxW;
    if (out.bbx_h <= 0) out.bbx_h = maxH;
  }
  out.bbx_xoff = 0;
  out.bbx_yoff = -out.descent;

  if (!out.boundsValid) out.computeBounds();

  result.ok = true;
  return result;
}


// ──────────────────────────────────────────────────────────────────────
// loadPcfGz — decompress .pcf.gz and parse
// ──────────────────────────────────────────────────────────────────────

BdfParseResult loadPcfGz(const std::string& path, BdfFont& out) {
  BdfParseResult result;

  gzFile gz = gzopen(path.c_str(), "rb");
  if (!gz) {
    result.error = "cannot open: " + path;
    return result;
  }

  // Read the entire decompressed file into a buffer.
  // Most PCF fonts are <200KB decompressed.
  std::vector<uint8_t> buf;
  buf.resize(256 * 1024); // 256KB initial
  size_t total = 0;

  while (true) {
    int n = gzread(gz, buf.data() + total, (unsigned)(buf.size() - total));
    if (n < 0) {
      gzclose(gz);
      result.error = "gzread error: " + path;
      return result;
    }
    if (n == 0) break; // EOF
    total += (size_t)n;
    if (total >= buf.size()) {
      buf.resize(buf.size() * 2);
      if (buf.size() > 4 * 1024 * 1024) {
        gzclose(gz);
        result.error = "PCF too large: " + path;
        return result;
      }
    }
  }
  gzclose(gz);

  buf.resize(total);
  return parsePcf(buf.data(), buf.size(), out);
}

} // namespace x11::font
