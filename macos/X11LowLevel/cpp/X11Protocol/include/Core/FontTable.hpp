//
//  FontTable.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

#include "Fonts/BDF.hpp"

namespace x11 {

/// Case-insensitive glob matching with * (any substring) and ? (single char).
/// Used for ListFonts XLFD pattern matching.
bool fontGlobMatch(const char* pattern, const char* str);

class FontTable {
public:
  FontTable() = default;

  // Load bundled fonts once (call from server init)
  bool loadBuiltins(std::string* err);

  // Load font aliases from system fonts.alias files
  void loadAliases();

  // Open/close are X11 protocol font IDs (fids)
  bool open(uint32_t fid, const std::string& name);
  void close(uint32_t fid);

  // Resolve fid -> font
  const x11::font::BdfFont* get(uint32_t fid) const;

  // Name lookup (e.g. "fixed", "cursor", or XLFD pattern)
  const x11::font::BdfFont* findByName(const std::string& name) const;

  // List all font names: short names, XLFD names, and aliases (for ListFonts)
  std::vector<std::string> listNames() const;

  // Erase per-client resources (font IDs owned by a specific client)
  void eraseOwnedBy(uint32_t clientBase, uint32_t clientMask);

private:
  // Built-in fonts by short name (e.g. "6x13")
  std::unordered_map<std::string, std::unique_ptr<x11::font::BdfFont>> builtins_;

  // Open font IDs -> pointer into builtins_
  std::unordered_map<uint32_t, const x11::font::BdfFont*> open_;

  // Alias name -> target font (e.g. "fixed" -> "-misc-fixed-...-iso8859-1")
  // Stores both system aliases (from fonts.alias) and our short-name aliases
  std::unordered_map<std::string, const x11::font::BdfFont*> aliases_;
};

} // namespace x11
