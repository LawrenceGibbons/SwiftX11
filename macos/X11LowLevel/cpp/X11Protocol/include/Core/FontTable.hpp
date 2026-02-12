//
//  FontTable.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>

#include "Fonts/BDF.hpp"

namespace x11 {

class FontTable {
public:
  FontTable() = default;

  // Load bundled fonts once (call from server init)
  bool loadBuiltins(std::string* err);

  // Open/close are X11 protocol font IDs (fids)
  bool open(uint32_t fid, const std::string& name);
  void close(uint32_t fid);

  // Resolve fid -> font
  const x11::font::BdfFont* get(uint32_t fid) const;

  // Name lookup (e.g. "fixed", "cursor")
  const x11::font::BdfFont* findByName(const std::string& name) const;

private:
  // Built-in fonts by name
  std::unordered_map<std::string, std::unique_ptr<x11::font::BdfFont>> builtins_;

  // Open font IDs -> pointer into builtins_
  std::unordered_map<uint32_t, const x11::font::BdfFont*> open_;
};

} // namespace x11
