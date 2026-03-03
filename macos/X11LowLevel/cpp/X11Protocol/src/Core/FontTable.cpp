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
#include "Fonts/BundleResource.hpp"

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

static const x11::font::BdfFont* pickClosestByPixelSize(
                                                        const std::unordered_map<std::string, std::unique_ptr<x11::font::BdfFont>>& builtins,
                                                        int pixel)
{
  if (pixel <= 0) return nullptr;

  // Candidate keys you ship (add/remove as you like)
  struct Cand { const char* key; int px; };
  const Cand cands[] = {
    {"6x13", 13},
    {"7x13", 13},
    {"8x13", 13},
    {"7x14", 14},
    {"9x15", 15},
    {"9x18", 18},
    {"10x20", 20},
    {"fixed", 13}, // treat fixed as 13-ish fallback
  };

  const x11::font::BdfFont* best = nullptr;
  int bestDist = 1<<30;

  for (const auto& c : cands) {
    auto it = builtins.find(c.key);
    if (it == builtins.end()) continue;
    int d = (c.px > pixel) ? (c.px - pixel) : (pixel - c.px);
    if (d < bestDist) { bestDist = d; best = it->second.get(); }
  }
  return best;
}


static void dbgFontResolve(const std::string& req,
                           const char* resolvedKey,
                           const x11::font::BdfFont* f)
{
#ifndef NDEBUG
  if (!f) {
    fprintf(stderr, "[FontTable] resolve req=\"%s\" -> %s (NULL)\n",
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


static const x11::font::BdfFont* pickClosestMonospaceByPixel(
    const std::unordered_map<std::string, std::unique_ptr<x11::font::BdfFont>>& builtins,
    int px)
{
  auto get = [&](const char* k) -> const x11::font::BdfFont* {
    auto it = builtins.find(std::string(k));
    return (it == builtins.end()) ? nullptr : it->second.get();
  };

  // If px is unknown, pick a readable default.
  if (px <= 0) {
    if (auto* f = get("9x15")) return f;
    return get("fixed");
  }

  // Exact-ish buckets (your actual set)
  if (px <= 13) {
    // Prefer 8x13 over 6x13 to avoid "tiny".
    if (auto* f = get("8x13")) return f;
    if (auto* f = get("7x13")) return f;
    if (auto* f = get("6x13")) return f;
    if (auto* f = get("fixed")) return f;
  }

  if (px == 14) {
    if (auto* f = get("7x14")) return f;
    // fall back to nearest
    if (auto* f = get("9x15")) return f;
    if (auto* f = get("8x13")) return f;
  }

  if (px >= 15 && px <= 17) {
    if (auto* f = get("9x15")) return f;
    if (auto* f = get("9x18")) return f;
  }

  if (px >= 18 && px <= 19) {
    if (auto* f = get("9x18")) return f;
    if (auto* f = get("10x20")) return f;
  }

  // px >= 20
  if (auto* f = get("10x20")) return f;

  // Final fallback
  if (auto* f = get("9x15")) return f;
  return get("fixed");
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

      // Try to find a builtin font that matches the target XLFD
      std::string lTarget = lower(target);
      const x11::font::BdfFont* match = nullptr;

      for (const auto& [key, font] : builtins_) {
        if (!font) continue;
        // Match against the font's XLFD name
        std::string lName = lower(font->name);
        if (lName == lTarget) {
          match = font.get();
          break;
        }
        // Also try glob matching (target may contain wildcards in alias files)
        if (fontGlobMatch(lTarget.c_str(), lName.c_str())) {
          match = font.get();
          // Don't break — keep looking for exact match
        }
      }

      if (match) {
        aliases_[lower(alias)] = match;
#ifndef NDEBUG
        fprintf(stderr, "[FontTable] alias \"%s\" -> \"%s\"\n",
                alias.c_str(), match->name.c_str());
#endif
      }
    }
  }

#ifndef NDEBUG
  fprintf(stderr, "[FontTable] loaded %zu aliases\n", aliases_.size());
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

  // XLFD glob matching: if the name contains wildcards, find the first match
  if (key.find('*') != std::string::npos || key.find('?') != std::string::npos) {
    for (const auto& [shortKey, font] : builtins_) {
      if (!font) continue;
      std::string lName = lower(font->name);
      if (fontGlobMatch(key.c_str(), lName.c_str())) {
        dbgFontResolve(name, shortKey.c_str(), font.get());
        return font.get();
      }
    }
  }

  // Minimal XLFD handling: extract pixel size and pick closest font
  if (!key.empty() && key[0] == '-') {
    int px = parseXLFD_PixelSize(key);
    int scaledPx = applyFontScale(px);
    const x11::font::BdfFont* f = pickClosestByBbxHeight(builtins_, scaledPx, "fixed");

  #ifndef NDEBUG
    fprintf(stderr, "[FontTable] XLFD px=%d scaled=%d -> \"%s\" (bbx_h=%d)\n",
            px, scaledPx, f ? f->name.c_str() : "<null>", f ? f->bbx_h : -1);
  #endif

    if (f) return f;
    // fall through to default
  }

#ifndef NDEBUG
  fprintf(stderr, "[FontTable] unknown font \"%s\" -> default\n", name.c_str());
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

  // Convert to sorted vector
  return std::vector<std::string>(nameSet.begin(), nameSet.end());
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
