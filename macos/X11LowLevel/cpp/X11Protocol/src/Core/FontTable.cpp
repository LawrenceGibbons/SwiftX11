//
//  FontTable.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#include <string>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <set>

#include "Core/FontTable.hpp"
#include "Fonts/BDF.hpp"
#include "Fonts/PCF.hpp"
#include "Fonts/CoreTextFont.hpp"
#include "Fonts/BundleResource.hpp"
#include "Utils/TraceDefs.hpp"
#include "Utils/MachTime.hpp"

namespace x11 {

// ──────────────────────────────────────────────────────────────────────
// Utility: case-insensitive glob matching with * and ?
// ──────────────────────────────────────────────────────────────────────

bool fontGlobMatch(const char* pat, const char* str) {
  // Iterative glob match to avoid recursion stack issues.
  // Uses the "star restart" technique: remember the position of the
  // last * and backtrack if a later literal match fails.
  const char* starPat = nullptr;
  const char* starStr = nullptr;

  while (*str) {
    if (*pat == '?') {
      // ? matches exactly one character
      pat++;
      str++;
    } else if (*pat == '*') {
      // * matches zero or more characters — record restart point
      starPat = ++pat;
      starStr = str;
    } else if (std::tolower((unsigned char)*pat) == std::tolower((unsigned char)*str)) {
      pat++;
      str++;
    } else if (starPat) {
      // Mismatch — backtrack to last * and consume one more char
      pat = starPat;
      str = ++starStr;
    } else {
      return false;
    }
  }

  // Consume trailing *'s in pattern
  while (*pat == '*') pat++;
  return *pat == '\0';
}


// ──────────────────────────────────────────────────────────────────────
// Internal helpers
// ──────────────────────────────────────────────────────────────────────

static std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}


// Returns pixel size (>=1) if parsed, else 0.
static int parseXLFD_PixelSize(const std::string& name) {
  if (name.empty()) return 0;
  if (name[0] != '-') return 0; // only treat real XLFDs here

  // Split on '-' while preserving the leading empty token.
  // Example: "-misc-fixed-..." => parts[0]=="" parts[1]=="misc" ... parts[7]==PIXEL_SIZE
  int field = 0;
  std::string token;
  token.reserve(32);

  auto flushToken = [&](int f, const std::string& t) -> int {
    // Field 7 (1-based after the leading '-') is PIXEL_SIZE.
    // With parts[0]=="" then parts[7] corresponds to PIXEL_SIZE.
    if (f == 7) {
      // Must be an integer (XLFD uses decimal).
      if (t.empty()) return 0;
      char* end = nullptr;
      long v = std::strtol(t.c_str(), &end, 10);
      if (end == t.c_str() || *end != '\0') return 0;
      if (v <= 0 || v > 1000) return 0;
      return (int)v;
    }
    return 0;
  };

  // field counts tokens between '-' characters. Start with field=0 for the leading empty token.
  // We want to evaluate field==7 when it is complete.
  for (size_t i = 0; i < name.size(); i++) {
    char c = name[i];
    if (c == '-') {
      int v = flushToken(field, token);
      if (v > 0) return v;
      token.clear();
      field++;
      continue;
    }
    token.push_back(c);
  }

  // Flush last token
  int v = flushToken(field, token);
  return v;
}

static int fontScaleNumer = 2; // 2x for Retina by default
static int fontScaleDenom = 1;

static int applyFontScale(int px) {
  if (px <= 0) return 0;
  // round(px * numer/denom)
  return (px * fontScaleNumer + fontScaleDenom/2) / fontScaleDenom;
}

static void dbgFontResolve(const std::string& req,
                           const char* resolvedKey,
                           const x11::font::BdfFont* f)
{
#if X11_TRACE_FONT_ENABLED
  if (!f) {
    TS_FPRINTF("[FontTable] resolve req=\"%s\" -> %s (NULL)\n",
            req.c_str(), resolvedKey ? resolvedKey : "<null>");
    return;
  }

  // Basic metrics that matter for terminal size
  fprintf(stderr,
          "[FontTable] resolve req=\"%s\" -> key=\"%s\" "
          "bbx=%dx%d off=%d,%d ascent=%d descent=%d defaultChar=%d glyphs=%zu\n",
          req.c_str(),
          resolvedKey ? resolvedKey : "<null>",
          f->bbx_w, f->bbx_h, f->bbx_xoff, f->bbx_yoff,
          f->ascent, f->descent, f->defaultChar,
          f->glyphs.size());
#endif
}


static const x11::font::BdfFont* pickClosestByBbxHeight(
    const std::unordered_map<std::string, std::unique_ptr<x11::font::BdfFont>>& builtins,
    int targetPx,
    const char* fallbackKey = "fixed")
{
  if (targetPx <= 0) {
    auto it = builtins.find(fallbackKey);
    return (it == builtins.end()) ? nullptr : it->second.get();
  }

  const x11::font::BdfFont* best = nullptr;
  int bestDist = 1<<30;

  // Consider only your monospace-ish roman fonts (exclude ja/ko here)
  const char* keys[] = {
    "6x13","7x13","8x13","7x14","9x15","9x18","10x20",
    "6x13B","6x13O","7x13B","7x13O","7x14B","8x13B","8x13O","9x15B","9x18B",
    "fixed"
  };

  for (const char* k : keys) {
    auto it = builtins.find(k);
    if (it == builtins.end()) continue;
    const auto* f = it->second.get();
    if (!f) continue;
    const int h = (f->bbx_h > 0) ? f->bbx_h : 0;
    if (h <= 0) continue;
    const int d = (h > targetPx) ? (h - targetPx) : (targetPx - h);
    if (d < bestDist) { bestDist = d; best = f; }
  }

  if (!best) {
    auto it = builtins.find(fallbackKey);
    return (it == builtins.end()) ? nullptr : it->second.get();
  }
  return best;
}


// ──────────────────────────────────────────────────────────────────────
// FontTable implementation
// ──────────────────────────────────────────────────────────────────────

bool FontTable::loadBuiltins(std::string* err) {
  auto loadBdf = [&](const char* nameKey, const char* fileBase) -> bool {
    // std::string text = loadFrameworkTextResource(fileBase, "bdf", nullptr);
    std::string text = loadFrameworkTextResource(fileBase, "bdf", "fonts");    if (text.empty()) {
      if (err) *err = std::string("missing resource: ") + fileBase + ".bdf";
    }

    auto font = std::make_unique<x11::font::BdfFont>();
    auto res = x11::font::parseBdf(text, *font);
    if (!res.ok) {
      if (err) *err = std::string("BDF parse failed for ") + fileBase + ": " + res.error;
      return false;
    }

    if (!font->boundsValid) font->computeBounds();
    builtins_[lower(nameKey)] = std::move(font);
    return true;
  };

  // Required baseline
  if (!loadBdf("fixed", "fixed")) return false;

  // Strongly recommended for usable terminals
  (void)loadBdf("6x13",    "6x13");
  (void)loadBdf("7x13",    "7x13");
  (void)loadBdf("8x13",    "8x13");
  (void)loadBdf("9x15",    "9x15");
  (void)loadBdf("10x20",   "10x20");
  (void)loadBdf("7x14",    "7x14");
  (void)loadBdf("9x18",    "9x18");
  (void)loadBdf("12x13ja", "12x13ja");
  (void)loadBdf("18x18ja", "18x18ja");
  (void)loadBdf("18x18ko", "18x18ko");
  (void)loadBdf("6x13B",   "6x13B");
  (void)loadBdf("6x13O",   "6x13O");
  (void)loadBdf("7x13B",   "7x13B");
  (void)loadBdf("7x13O",   "7x13O");
  (void)loadBdf("7x14B",   "7x14B");
  (void)loadBdf("8x13B",   "8x13B");
  (void)loadBdf("8x13O",   "8x13O");
  (void)loadBdf("9x15B",   "9x15B");
  (void)loadBdf("9x18B",   "9x18B");
  (void)loadBdf("fixed",   "fixed");

  // Scan system font directories (fonts.dir) for PCF font names
  // This only registers XLFD names → file paths; actual fonts are loaded lazily.
  const char* fontDirs[] = {
    "/opt/X11/share/fonts/misc",
    "/opt/X11/share/fonts/75dpi",
    "/opt/X11/share/fonts/100dpi",
  };

  for (const char* dir : fontDirs) {
    std::string dirPath(dir);
    std::string fontsDir = dirPath + "/fonts.dir";
    std::ifstream dirFile(fontsDir);
    if (!dirFile.is_open()) {
#if X11_TRACE_FONT_ENABLED
      TS_FPRINTF("[FontTable] FAILED to open: %s (errno=%d)\n",
              fontsDir.c_str(), errno);
#endif
      continue;
    }

    std::string line;
    // First line is the count — skip it
    if (!std::getline(dirFile, line)) {
#if X11_TRACE_FONT_ENABLED
      TS_FPRINTF("[FontTable] empty fonts.dir: %s\n", fontsDir.c_str());
#endif
      continue;
    }
#if X11_TRACE_FONT_ENABLED
    TS_FPRINTF("[FontTable] scanning: %s (count line: \"%s\")\n",
            fontsDir.c_str(), line.c_str());
#endif

    while (std::getline(dirFile, line)) {
      if (line.empty()) continue;
      // Format: "filename.pcf.gz -foundry-family-weight-slant-..."
      size_t sp = line.find(' ');
      if (sp == std::string::npos) continue;

      std::string filename = line.substr(0, sp);
      std::string xlfd = line.substr(sp + 1);
      if (xlfd.empty() || xlfd[0] != '-') continue;

      std::string fullPath = dirPath + "/" + filename;
      std::string xlfdLower = lower(xlfd);

      // Don't register if we already have this font as a builtin
      bool alreadyBuiltin = false;
      for (const auto& [key, font] : builtins_) {
        if (font && lower(font->name) == xlfdLower) {
          alreadyBuiltin = true;
          break;
        }
      }
      if (alreadyBuiltin) continue;

      pcfRegistry_[xlfdLower] = fullPath;
    }
  }

#ifndef NDEBUG
  TS_FPRINTF("[FontTable] registered %zu PCF fonts, ",
          pcfRegistry_.size());
#endif

  // Load system font aliases (fonts.alias files)
  loadAliases();

  return true;
}


void FontTable::loadAliases() {
  // Standard alias directories from XQuartz installation
  const char* aliasDirs[] = {
    "/opt/X11/share/fonts/misc/fonts.alias",
    "/opt/X11/share/fonts/75dpi/fonts.alias",
    "/opt/X11/share/fonts/100dpi/fonts.alias",
  };

  for (const char* path : aliasDirs) {
    std::ifstream file(path);
    if (!file.is_open()) continue;

    std::string line;
    while (std::getline(file, line)) {
      // Skip comments and empty lines
      if (line.empty() || line[0] == '!') continue;

      // Format: aliasName<whitespace>targetXLFD
      // Some files use quotes around names with spaces
      std::istringstream iss(line);
      std::string alias, target;
      iss >> alias >> target;
      if (alias.empty() || target.empty()) continue;

      // Remove quotes if present
      if (alias.front() == '"' && alias.back() == '"' && alias.size() >= 2)
        alias = alias.substr(1, alias.size() - 2);

      // Try to find a font that matches the target XLFD
      // First check builtins, then PCF registry (lazy load)
      std::string lTarget = lower(target);
      const x11::font::BdfFont* match = nullptr;

      // Check builtins (exact match first, then glob)
      for (const auto& [key, font] : builtins_) {
        if (!font) continue;
        std::string lName = lower(font->name);
        if (lName == lTarget) {
          match = font.get();
          break;
        }
        if (fontGlobMatch(lTarget.c_str(), lName.c_str())) {
          match = font.get();
          // Don't break — keep looking for exact match
        }
      }

      // If no builtin match, try PCF registry (exact then glob)
      if (!match) {
        auto pcfIt = pcfRegistry_.find(lTarget);
        if (pcfIt != pcfRegistry_.end()) {
          match = loadPcfOnDemand(lTarget);
        } else {
          // Glob match against PCF registry
          for (const auto& [xlfd, path] : pcfRegistry_) {
            if (fontGlobMatch(lTarget.c_str(), xlfd.c_str())) {
              match = loadPcfOnDemand(xlfd);
              if (match) break;
            }
          }
        }
      }

      if (match) {
        aliases_[lower(alias)] = match;
#if X11_TRACE_FONT_ENABLED
        TS_FPRINTF("[FontTable] alias \"%s\" -> \"%s\"\n",
                alias.c_str(), match->name.c_str());
#endif
      }
    }
  }

#ifndef NDEBUG
  fprintf(stderr, "%zu aliases\n", aliases_.size());
#endif
}


bool FontTable::open(uint32_t fid, const std::string& name) {
  if (fid == 0) return false;
  const auto* f = findByName(name);
  if (!f) return false;
  open_[fid] = f;
  return true;
}

void FontTable::close(uint32_t fid) {
  open_.erase(fid);
}

const x11::font::BdfFont* FontTable::get(uint32_t fid) const {
  auto it = open_.find(fid);
  return (it == open_.end()) ? nullptr : it->second;
}

const x11::font::BdfFont* FontTable::findByName(const std::string& name) const {
  std::string key = lower(name);

  auto get = [&](const char* k) -> const x11::font::BdfFont* {
    auto it = builtins_.find(std::string(k));
    return (it == builtins_.end()) ? nullptr : it->second.get();
  };

  auto defaultFont = [&]() -> std::pair<const char*, const x11::font::BdfFont*> {
    if (auto* f = get("9x15"))  return {"9x15", f};
    if (auto* f = get("10x20")) return {"10x20", f};
    if (auto* f = get("6x13"))  return {"6x13", f};
    if (auto* f = get("fixed")) return {"fixed", f};
    return {"fixed", nullptr};
  };

  // libX11/libXt "default font" tokens
  if (key.empty() || key == "nil" || key == "nil2") {
    auto [rk, f] = defaultFont();
    dbgFontResolve(name, rk, f);
    return f;
  }

  // Cursor font (until you ship cursor.bdf)
  if (key == "cursor") {
    auto [rk, f] = defaultFont();
    dbgFontResolve(name, rk, f);
    return f;
  }

  // Exact match by short name (e.g. "6x13", "9x15B")
  if (auto it = builtins_.find(key); it != builtins_.end()) {
    dbgFontResolve(name, key.c_str(), it->second.get());
    return it->second.get();
  }

  // Check aliases (e.g. "fixed" -> system XLFD, "variable" -> helvetica-ish)
  if (auto it = aliases_.find(key); it != aliases_.end()) {
    dbgFontResolve(name, "alias", it->second);
    return it->second;
  }

  // Exact match by XLFD name (e.g. "-misc-fixed-medium-r-normal--20-200-75-75-c-100-iso10646-1")
  for (const auto& [shortKey, font] : builtins_) {
    if (!font) continue;
    if (lower(font->name) == key) {
      dbgFontResolve(name, shortKey.c_str(), font.get());
      return font.get();
    }
  }

  // Check PCF registry for exact XLFD match (lazy load)
  if (auto it = pcfRegistry_.find(key); it != pcfRegistry_.end()) {
    const auto* f = loadPcfOnDemand(key);
    if (f) {
      dbgFontResolve(name, "pcf", f);
      return f;
    }
  }

  // Try CoreText (macOS system fonts) — before glob/pixel-size fallbacks
  if (const auto* f = tryLoadCoreText(key)) {
    dbgFontResolve(name, "coretext", f);
    return f;
  }

  // XLFD glob matching: if the name contains wildcards, find the first match
  if (key.find('*') != std::string::npos || key.find('?') != std::string::npos) {
#if X11_TRACE_FONT_ENABLED
    TS_FPRINTF("[FontTable] GLOB search for \"%s\" (pcfRegistry_=%zu entries)\n",
            key.c_str(), pcfRegistry_.size());
#endif
    // First check builtins
    for (const auto& [shortKey, font] : builtins_) {
      if (!font) continue;
      std::string lName = lower(font->name);
      if (fontGlobMatch(key.c_str(), lName.c_str())) {
#if X11_TRACE_FONT_ENABLED
        TS_FPRINTF("[FontTable] GLOB matched BUILTIN \"%s\" (xlfd=\"%s\")\n",
                shortKey.c_str(), lName.c_str());
#endif
        dbgFontResolve(name, shortKey.c_str(), font.get());
        return font.get();
      }
    }
    // Then check PCF registry
    int pcfChecked = 0;
    for (const auto& [xlfd, path] : pcfRegistry_) {
      if (fontGlobMatch(key.c_str(), xlfd.c_str())) {
#if X11_TRACE_FONT_ENABLED
        TS_FPRINTF("[FontTable] GLOB matched PCF \"%s\" (checked %d entries)\n",
                xlfd.c_str(), pcfChecked);
#endif
        const auto* f = loadPcfOnDemand(xlfd);
        if (f) {
          dbgFontResolve(name, "pcf-glob", f);
          return f;
        }
#if X11_TRACE_FONT_ENABLED
        TS_FPRINTF("[FontTable] GLOB PCF load FAILED for \"%s\"\n", xlfd.c_str());
#endif
      }
      pcfChecked++;
    }
#if X11_TRACE_FONT_ENABLED
    TS_FPRINTF("[FontTable] GLOB no match found after checking %d PCF entries\n", pcfChecked);
#endif
  }

  // Minimal XLFD handling: extract pixel size and pick closest font
  if (!key.empty() && key[0] == '-') {
    int px = parseXLFD_PixelSize(key);
    int scaledPx = applyFontScale(px);
    const x11::font::BdfFont* f = pickClosestByBbxHeight(builtins_, scaledPx, "fixed");

  #if X11_TRACE_FONT_ENABLED
    TS_FPRINTF("[FontTable] XLFD px=%d scaled=%d -> \"%s\" (bbx_h=%d)\n",
            px, scaledPx, f ? f->name.c_str() : "<null>", f ? f->bbx_h : -1);
  #endif

    if (f) return f;
    // fall through to default
  }

#if X11_TRACE_FONT_ENABLED
  TS_FPRINTF("[FontTable] unknown font \"%s\" -> default\n", name.c_str());
#endif
  auto [rk, f] = defaultFont();
  dbgFontResolve(name, rk, f);
  return f;
}


std::vector<std::string> FontTable::listNames() const {
  // Return both short names AND full XLFD names for each font.
  // This matches standard X11 server behavior where ListFonts
  // returns both alias names and full XLFD names.
  std::set<std::string> nameSet;

  for (const auto& [shortKey, font] : builtins_) {
    // Add the short name (e.g. "6x13")
    nameSet.insert(shortKey);

    // Add the full XLFD name from the BDF FONT line
    if (font && !font->name.empty()) {
      nameSet.insert(lower(font->name));
    }
  }

  // Add alias names
  for (const auto& [alias, _] : aliases_) {
    nameSet.insert(alias);
  }

  // Add PCF registry XLFD names (system fonts)
  for (const auto& [xlfd, _] : pcfRegistry_) {
    nameSet.insert(xlfd);
  }

  // Convert to sorted vector
  return std::vector<std::string>(nameSet.begin(), nameSet.end());
}


const x11::font::BdfFont* FontTable::loadPcfOnDemand(const std::string& xlfdLower) const {
  // Check cache first
  auto cacheIt = pcfCache_.find(xlfdLower);
  if (cacheIt != pcfCache_.end()) {
    return cacheIt->second.get();
  }

  // Find the path in the registry
  auto regIt = pcfRegistry_.find(xlfdLower);
  if (regIt == pcfRegistry_.end()) return nullptr;

  const std::string& path = regIt->second;

  auto font = std::make_unique<x11::font::BdfFont>();
  auto res = x11::font::loadPcfGz(path, *font);
  if (!res.ok) {
#if X11_TRACE_FONT_ENABLED
    TS_FPRINTF("[FontTable] PCF load failed: %s -> %s\n",
            xlfdLower.c_str(), res.error.c_str());
#endif
    return nullptr;
  }

  if (!font->boundsValid) font->computeBounds();

#if X11_TRACE_FONT_ENABLED
  TS_FPRINTF("[FontTable] PCF loaded: \"%s\" bbx=%dx%d ascent=%d descent=%d glyphs=%zu\n",
          font->name.c_str(), font->bbx_w, font->bbx_h,
          font->ascent, font->descent, font->glyphs.size());
#endif

  const auto* ptr = font.get();
  pcfCache_[xlfdLower] = std::move(font);
  return ptr;
}


const x11::font::BdfFont* FontTable::tryLoadCoreText(const std::string& nameLower) const {
  // Check cache first
  auto cacheIt = ctCache_.find(nameLower);
  if (cacheIt != ctCache_.end()) {
    return cacheIt->second.get(); // may be nullptr (negative cache)
  }

  // Skip fonts that must use PCF/BDF (special encodings)
  if (nameLower == "cursor") return nullptr;
  if (nameLower.find("fontspecific") != std::string::npos) return nullptr;
  if (nameLower.find("symbol") != std::string::npos) return nullptr;

  // Skip wildcard patterns — CoreText needs a concrete name
  if (nameLower.find('*') != std::string::npos ||
      nameLower.find('?') != std::string::npos) {
    return nullptr;
  }

  // Extract pixel size from XLFD if present
  int pixelSize = 0;
  if (!nameLower.empty() && nameLower[0] == '-') {
    pixelSize = parseXLFD_PixelSize(nameLower);
  }
  if (pixelSize <= 0) pixelSize = 13; // sensible default

  auto font = x11::font::createCoreTextFont(nameLower, pixelSize);
  if (!font || font->glyphs.empty()) {
    // Negative cache: don't retry
    ctCache_[nameLower] = nullptr;
    return nullptr;
  }

  const auto* ptr = font.get();
  ctCache_[nameLower] = std::move(font);
  return ptr;
}


void FontTable::eraseOwnedBy(uint32_t clientBase, uint32_t clientMask) {
  // Close any font IDs that belong to this client
  auto it = open_.begin();
  while (it != open_.end()) {
    if ((it->first & ~clientMask) == (clientBase & ~clientMask)) {
      it = open_.erase(it);
    } else {
      ++it;
    }
  }
}

} // namespace x11
