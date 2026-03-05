//
//  CoreTextFont.cpp
//  X11LowLevel
//
//  CoreText bridge implementation.
//  Uses CoreText + CoreGraphics C APIs to rasterize system fonts.
//

#include "Fonts/CoreTextFont.hpp"
#include "Utils/TraceDefs.hpp"

#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

namespace x11::font {

// ──────────────────────────────────────────────────────────────────────
// Antialiased fonts toggle (atomic for thread safety)
// ──────────────────────────────────────────────────────────────────────

static std::atomic<bool> g_antialiased{true};

bool antialiasedFonts() { return g_antialiased.load(std::memory_order_relaxed); }
void setAntialiasedFonts(bool enabled) { g_antialiased.store(enabled, std::memory_order_relaxed); }


// ──────────────────────────────────────────────────────────────────────
// XLFD parsing
// ──────────────────────────────────────────────────────────────────────

struct XLFDFields {
  std::string foundry;
  std::string family;
  std::string weight;   // "medium", "bold", "demibold", etc.
  std::string slant;    // "r", "i", "o"
  int pixelSize = 0;
  int pointSize = 0;
  std::string charsetRegistry;
  std::string charsetEncoding;
};

static std::string toLower(const std::string& s) {
  std::string r = s;
  for (char& c : r) c = (char)std::tolower((unsigned char)c);
  return r;
}

static XLFDFields parseXLFD(const std::string& xlfd) {
  XLFDFields f;
  if (xlfd.empty() || xlfd[0] != '-') return f;

  // Split on '-': parts[0]="" parts[1]=foundry parts[2]=family ...
  std::vector<std::string> parts;
  std::string tok;
  for (char c : xlfd) {
    if (c == '-') {
      parts.push_back(tok);
      tok.clear();
    } else {
      tok.push_back(c);
    }
  }
  parts.push_back(tok);

  // XLFD has 14 fields after the leading dash (15 parts total with parts[0]="")
  if (parts.size() >= 3)  f.foundry = toLower(parts[1]);
  if (parts.size() >= 3)  f.family  = toLower(parts[2]);
  if (parts.size() >= 4)  f.weight  = toLower(parts[3]);
  if (parts.size() >= 5)  f.slant   = toLower(parts[4]);
  if (parts.size() >= 8) {
    // parts[7] = pixel size
    char* end = nullptr;
    long v = std::strtol(parts[7].c_str(), &end, 10);
    if (end != parts[7].c_str() && v > 0 && v < 1000)
      f.pixelSize = (int)v;
  }
  if (parts.size() >= 9) {
    char* end = nullptr;
    long v = std::strtol(parts[8].c_str(), &end, 10);
    if (end != parts[8].c_str() && v > 0)
      f.pointSize = (int)v;
  }
  if (parts.size() >= 14) f.charsetRegistry = toLower(parts[13]);
  if (parts.size() >= 15) f.charsetEncoding = toLower(parts[14]);

  return f;
}


// ──────────────────────────────────────────────────────────────────────
// X11 family name → macOS font name mapping
// ──────────────────────────────────────────────────────────────────────

struct FamilyMapping {
  const char* x11Name;
  const char* macName;
};

static const FamilyMapping kFamilyMap[] = {
  {"fixed",       "Menlo"},
  {"courier",     "Courier"},
  {"helvetica",   "Helvetica"},
  {"times",       "Times New Roman"},
  {"lucida",      "Lucida Grande"},
  {"lucidatypewriter", "Menlo"},
  {"clean",       "Menlo"},
  {"terminal",    "Menlo"},
  {"misc-fixed",  "Menlo"},
  {"charter",     "Charter"},
  {"new century schoolbook", "Times New Roman"},
  {"palatino",    "Palatino"},
  {"avantgarde",  "Avenir"},
  {"bookman",     "Georgia"},
  {nullptr, nullptr}
};

static const char* mapFamily(const std::string& x11Family) {
  std::string key = toLower(x11Family);
  for (const auto* m = kFamilyMap; m->x11Name; m++) {
    if (key == m->x11Name) return m->macName;
  }
  return nullptr;
}


// ──────────────────────────────────────────────────────────────────────
// CoreText font creation
// ──────────────────────────────────────────────────────────────────────

static CTFontRef createCTFont(const std::string& family, int pixelSize,
                               bool bold, bool italic)
{
  // Map X11 family to macOS name
  const char* macFamily = mapFamily(family);
  std::string resolvedFamily = macFamily ? macFamily : family;

  // Create font name CFString
  CFStringRef cfFamily = CFStringCreateWithCString(
      kCFAllocatorDefault, resolvedFamily.c_str(), kCFStringEncodingUTF8);
  if (!cfFamily) return nullptr;

  // Build traits
  CTFontSymbolicTraits traits = 0;
  if (bold)   traits |= kCTFontBoldTrait;
  if (italic) traits |= kCTFontItalicTrait;

  CTFontRef font = nullptr;

  if (traits != 0) {
    // Create with traits via descriptor
    CFMutableDictionaryRef traitDict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    CFNumberRef traitVal = CFNumberCreate(kCFAllocatorDefault,
                                          kCFNumberSInt32Type, &traits);
    CFDictionarySetValue(traitDict, kCTFontSymbolicTrait, traitVal);

    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 2,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCTFontFamilyNameAttribute, cfFamily);
    CFDictionarySetValue(attrs, kCTFontTraitsAttribute, traitDict);

    CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
    font = CTFontCreateWithFontDescriptor(desc, (CGFloat)pixelSize, nullptr);

    CFRelease(desc);
    CFRelease(attrs);
    CFRelease(traitVal);
    CFRelease(traitDict);
  } else {
    // Simple creation by name
    font = CTFontCreateWithName(cfFamily, (CGFloat)pixelSize, nullptr);
  }

  CFRelease(cfFamily);

  // Verify we got a real font (not a fallback for an unknown name)
  if (font) {
    CFStringRef actualFamily = CTFontCopyFamilyName(font);
    if (actualFamily) {
      // Accept the font — CoreText always returns something
      CFRelease(actualFamily);
    }
  }

  return font;
}


// ──────────────────────────────────────────────────────────────────────
// Glyph rasterization
// ──────────────────────────────────────────────────────────────────────

static bool rasterizeGlyph(CTFontRef ctFont, UniChar ch, Glyph& out) {
  // Get glyph ID for this character
  CGGlyph glyphID = 0;
  UniChar chars[1] = { ch };
  if (!CTFontGetGlyphsForCharacters(ctFont, chars, &glyphID, 1)) {
    return false;  // No glyph for this character
  }
  if (glyphID == 0) return false;

  // Get bounding rect
  CGRect bbox = CTFontGetBoundingRectsForGlyphs(
      ctFont, kCTFontOrientationDefault, &glyphID, nullptr, 1);

  // Get advance
  CGSize advance = {};
  CTFontGetAdvancesForGlyphs(
      ctFont, kCTFontOrientationDefault, &glyphID, &advance, 1);

  // The bbox origin is relative to the glyph origin (baseline, pen position).
  // bbox.origin.x = left bearing (can be negative for overhanging glyphs)
  // bbox.origin.y = descent below baseline (negative means below)
  //
  // Compute integer pixel span from floor(origin) to ceil(origin + size).
  // This ensures fractional origins don't cause the buffer to be 1px too small
  // (e.g., origin.y = -3.2, height = 11.7 → span = ceil(8.5) - floor(-3.2) = 13,
  //  not ceil(11.7) = 12, which would clip the bottom of descenders).
  int x0 = (int)std::floor(bbox.origin.x);
  int y0 = (int)std::floor(bbox.origin.y);
  int x1 = (int)std::ceil(bbox.origin.x + bbox.size.width);
  int y1 = (int)std::ceil(bbox.origin.y + bbox.size.height);
  int gw = std::max(1, x1 - x0);
  int gh = std::max(1, y1 - y0);
  int bbx_xoff = x0;
  int bbx_yoff = y0;

  // Rasterize into an 8-bit grayscale bitmap context
  size_t bytesPerRow = (size_t)gw;  // 1 byte per pixel for 8-bit gray
  std::vector<uint8_t> buffer((size_t)gw * (size_t)gh, 0);

  CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
  CGContextRef cgCtx = CGBitmapContextCreate(
      buffer.data(), (size_t)gw, (size_t)gh, 8, bytesPerRow,
      gray, kCGImageAlphaNone);
  CGColorSpaceRelease(gray);

  if (!cgCtx) return false;

  // Enable antialiasing
  CGContextSetShouldAntialias(cgCtx, true);
  CGContextSetShouldSmoothFonts(cgCtx, true);
  CGContextSetAllowsFontSmoothing(cgCtx, true);
  CGContextSetAllowsAntialiasing(cgCtx, true);

  // Set foreground to white (on black background)
  CGContextSetGrayFillColor(cgCtx, 1.0, 1.0);

  // Draw the glyph.
  // CGContext is bottom-up. We draw at the position that places the
  // glyph's bounding box at (0, 0) in our buffer.
  // The glyph origin (baseline, pen position) needs to be at:
  //   x = -bbx_xoff (so the left edge of bbox maps to x=0)
  //   y = -bbx_yoff (so the bottom of bbox maps to y=0)
  CGPoint position = CGPointMake(-(CGFloat)bbx_xoff, -(CGFloat)bbx_yoff);
  CTFontDrawGlyphs(ctFont, &glyphID, &position, 1, cgCtx);

  CGContextRelease(cgCtx);

  // The buffer is bottom-up (CoreGraphics convention).
  // Flip vertically for X11 (top-down).
  std::vector<uint8_t> flipped((size_t)gw * (size_t)gh);
  for (int y = 0; y < gh; y++) {
    std::memcpy(flipped.data() + (size_t)y * (size_t)gw,
                buffer.data() + (size_t)(gh - 1 - y) * (size_t)gw,
                (size_t)gw);
  }

  // Fill the Glyph struct
  out.encoding = (int)ch;
  out.bbx_w = gw;
  out.bbx_h = gh;
  out.bbx_xoff = bbx_xoff;
  out.bbx_yoff = bbx_yoff;
  out.dwidth = (int)std::round(advance.width);
  if (out.dwidth <= 0) out.dwidth = gw;

  // Store 8-bit alpha coverage
  out.alpha = std::move(flipped);

  // Generate 1-bit bitmap (threshold at 128) for non-AA mode
  const int rowStride = (gw + 7) / 8;
  out.bitmap.assign((size_t)rowStride * (size_t)gh, 0);
  for (int y = 0; y < gh; y++) {
    for (int x = 0; x < gw; x++) {
      uint8_t a = out.alpha[(size_t)y * (size_t)gw + (size_t)x];
      if (a >= 128) {
        // MSBFirst bit packing (matching BDF convention)
        out.bitmap[(size_t)y * (size_t)rowStride + ((size_t)x >> 3)] |=
            (uint8_t)(1u << (7u - ((unsigned)x & 7u)));
      }
    }
  }

  return true;
}


// ──────────────────────────────────────────────────────────────────────
// Public API: create a BdfFont from CoreText
// ──────────────────────────────────────────────────────────────────────

std::unique_ptr<BdfFont> createCoreTextFont(const std::string& xlfdOrName,
                                             int pixelSize)
{
  std::string family;
  bool bold = false;
  bool italic = false;
  int pxSize = pixelSize;

  if (!xlfdOrName.empty() && xlfdOrName[0] == '-') {
    // XLFD string
    XLFDFields fields = parseXLFD(xlfdOrName);
    family = fields.family;
    if (family.empty() || family == "*") family = "fixed";

    bold = (fields.weight == "bold" || fields.weight == "demibold" ||
            fields.weight == "extrabold" || fields.weight == "black");
    italic = (fields.slant == "i" || fields.slant == "o");

    if (fields.pixelSize > 0) pxSize = fields.pixelSize;
    else if (fields.pointSize > 0) pxSize = (fields.pointSize + 5) / 10; // decipoints → pixels
  } else {
    // Bare name
    family = xlfdOrName;
  }

  if (pxSize <= 0) pxSize = 13;  // sensible default

  // Create the CoreText font
  CTFontRef ctFont = createCTFont(family, pxSize, bold, italic);
  if (!ctFont) {
#if X11_TRACE_FONT_ENABLED
    fprintf(stderr, "[CoreText] FAILED to create font: family=\"%s\" px=%d bold=%d italic=%d\n",
            family.c_str(), pxSize, bold ? 1 : 0, italic ? 1 : 0);
#endif
    return nullptr;
  }

  auto font = std::make_unique<BdfFont>();
  font->name = xlfdOrName;
  font->pointSize = pxSize; // approximate

  // Get font metrics.
  // CoreText separates line spacing into ascent + descent + leading.
  // X11 fonts only have ascent + descent, so we fold the leading into
  // ascent to get proper inter-line spacing (otherwise text is cramped).
  CGFloat ctAscent  = CTFontGetAscent(ctFont);
  CGFloat ctDescent = CTFontGetDescent(ctFont);
  CGFloat ctLeading = CTFontGetLeading(ctFont);
  font->ascent  = (int)std::ceil(ctAscent + ctLeading);
  font->descent = (int)std::ceil(ctDescent);

  // Font bounding box (approximate from cell size)
  font->bbx_h = font->ascent + font->descent;
  font->bbx_yoff = -(font->descent);

  // Rasterize Latin-1 glyphs (0-255)
  int maxWidth = 0;
  int maxGlyphDescent = 0;
  for (int ch = 0; ch < 256; ch++) {
    Glyph g;
    if (rasterizeGlyph(ctFont, (UniChar)ch, g)) {
      if (g.dwidth > maxWidth) maxWidth = g.dwidth;
      // Track the deepest glyph descent (-bbx_yoff = pixels below baseline)
      int d = -g.bbx_yoff;
      if (d > maxGlyphDescent) maxGlyphDescent = d;
      font->glyphs[ch] = std::move(g);
    }
  }

  // Ensure font descent covers all actual glyph extents.
  // CoreText's descent metric sometimes equals the deepest glyph extent
  // exactly, but X11 line spacing (ascent + descent) means the next line's
  // background clear starts at baseline + descent, overwriting the bottom
  // row of descenders if descent == maxGlyphDescent.  Add 1 pixel margin.
  if (maxGlyphDescent >= font->descent) {
    font->descent = maxGlyphDescent + 1;
  }

  // Update bounding box after descent adjustment
  font->bbx_h = font->ascent + font->descent;
  font->bbx_yoff = -(font->descent);

  // Set defaultChar (space if available, else first glyph)
  font->defaultChar = font->glyphs.count(32) ? 32 : 0;

  // Set font bounding box width
  font->bbx_w = maxWidth > 0 ? maxWidth : pxSize;
  font->bbx_xoff = 0;

  font->computeBounds();

  CFRelease(ctFont);

#if X11_TRACE_FONT_ENABLED
  fprintf(stderr, "[CoreText] created font: \"%s\" family=\"%s\" px=%d "
          "ascent=%d descent=%d bbx=%dx%d glyphs=%zu bold=%d italic=%d\n",
          xlfdOrName.c_str(), family.c_str(), pxSize,
          font->ascent, font->descent, font->bbx_w, font->bbx_h,
          font->glyphs.size(), bold ? 1 : 0, italic ? 1 : 0);
#endif

  return font;
}

} // namespace x11::font
