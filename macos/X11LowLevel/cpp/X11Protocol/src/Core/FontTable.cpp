//
//  FontTable.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#include "Core/FontTable.hpp"
#include "Fonts/BDF.hpp"
#include "Fonts/BundleResource.hpp"

namespace x11 {

static std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool FontTable::loadBuiltins(std::string* err) {
  auto loadBdf = [&](const char* nameKey, const char* fileBase) -> bool {
//    std::string text = loadFrameworkTextResource(fileBase, "bdf", "fonts");
    std::string text = loadFrameworkTextResource(fileBase, "bdf", nullptr);
    if (text.empty()) {
//      if (err) *err = std::string("missing resource: fonts/") + fileBase + ".bdf";
      if (err) *err = "missing resource: fixed.bdf";
      return false;
    }

    // xxx temp
    if (!text.empty()) {
      // optional: print first line
      auto nl = text.find('\n');
      std::string first = (nl == std::string::npos) ? text : text.substr(0, nl);
      fprintf(stderr, "[FontTable] loaded %s (%zu bytes): %s\n", fileBase, text.size(), first.c_str());
    }
    
    
    auto font = std::make_unique<x11::font::BdfFont>();
    auto res = x11::font::parseBdf(text, *font);
    if (!res.ok) {
      if (err) *err = std::string("BDF parse failed for ") + fileBase + ": " + res.error;

      // Helpful diagnostic
      auto nl = text.find('\n');
      std::string first = (nl == std::string::npos) ? text : text.substr(0, nl);
      fprintf(stderr, "[FontTable] parse failed; first line: %s\n", first.c_str());
      return false;
    }

    fprintf(stderr, "[FontTable] loaded %s OK (%zu glyphs)\n", fileBase, font->glyphs.size());
    
    
    if (!font->boundsValid) font->computeBounds();
    builtins_[lower(nameKey)] = std::move(font);
    return true;
  };

  // Minimum: fixed
  if (!loadBdf("fixed", "fixed")) return false;

  // Optional later:
  // loadBdf("cursor", "cursor");

  return true;
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

  // Common aliases xterm tries
  if (key == "6x13" || key == "7x13" || key == "9x15") key = "fixed";
  if (key == "cursor") key = "fixed"; // temporary until you embed cursor.bdf

  auto it = builtins_.find(key);
  if (it != builtins_.end()) return it->second.get();

  // Fallback: treat any unknown font name as "fixed" during bring-up
#ifndef NDEBUG
  fprintf(stderr, "[FontTable] unknown font \"%s\" -> fixed\n", name.c_str());
#endif
  auto it2 = builtins_.find("fixed");
  return (it2 == builtins_.end()) ? nullptr : it2->second.get();}

} // namespace x11
