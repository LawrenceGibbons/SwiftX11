//
//  AtomTable.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#include "Core/AtomTable.hpp"
#include "Core/ClipboardAtoms.hpp"

#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>

namespace x11 {

// Core protocol predefined atoms are 1..68 (XA_LAST_PREDEFINED = 68).
static constexpr uint32_t kFirstDynamicAtom = 69;

// Predefined atom names indexed by (atom_id - 1). Exactly 68 entries.
static const char* const kPredef[68] = {
  "PRIMARY",
  "SECONDARY",
  "ARC",
  "ATOM",
  "BITMAP",
  "CARDINAL",
  "COLORMAP",
  "CURSOR",
  "CUT_BUFFER0",
  "CUT_BUFFER1",
  "CUT_BUFFER2",
  "CUT_BUFFER3",
  "CUT_BUFFER4",
  "CUT_BUFFER5",
  "CUT_BUFFER6",
  "CUT_BUFFER7",
  "DRAWABLE",
  "FONT",
  "INTEGER",
  "PIXMAP",
  "POINT",
  "RECTANGLE",
  "RESOURCE_MANAGER",
  "RGB_COLOR_MAP",
  "RGB_BEST_MAP",
  "RGB_BLUE_MAP",
  "RGB_DEFAULT_MAP",
  "RGB_GRAY_MAP",
  "RGB_GREEN_MAP",
  "RGB_RED_MAP",
  "STRING",
  "VISUALID",
  "WINDOW",
  "WM_COMMAND",
  "WM_HINTS",
  "WM_CLIENT_MACHINE",
  "WM_ICON_NAME",
  "WM_ICON_SIZE",
  "WM_NAME",
  "WM_NORMAL_HINTS",
  "WM_SIZE_HINTS",
  "WM_ZOOM_HINTS",
  "MIN_SPACE",
  "NORM_SPACE",
  "MAX_SPACE",
  "END_SPACE",
  "SUPERSCRIPT_X",
  "SUPERSCRIPT_Y",
  "SUBSCRIPT_X",
  "SUBSCRIPT_Y",
  "UNDERLINE_POSITION",
  "UNDERLINE_THICKNESS",
  "STRIKEOUT_ASCENT",
  "STRIKEOUT_DESCENT",
  "ITALIC_ANGLE",
  "X_HEIGHT",
  "QUAD_WIDTH",
  "WEIGHT",
  "POINT_SIZE",
  "RESOLUTION",
  "COPYRIGHT",
  "NOTICE",
  "FONT_NAME",
  "FAMILY_NAME",
  "FULL_NAME",
  "CAP_HEIGHT",
  "WM_CLASS",
  "WM_TRANSIENT_FOR",
};

// Thread-local scratch storage for name() return pointers (avoids races).
static thread_local std::string tls_scratch;

struct AtomTableState {
  std::mutex mu;
  uint32_t next_atom = kFirstDynamicAtom;

  std::unordered_map<std::string, uint32_t> name_to_atom;
  std::unordered_map<uint32_t, std::string> atom_to_name;

  AtomTableState() {
    // Seed name_to_atom_ with predefined atoms so intern() can find them.
    for (uint32_t i = 1; i <= 68; i++) {
      name_to_atom.emplace(std::string(kPredef[i - 1]), i);
    }
    // Pre-register commonly used dynamic atoms at well-known IDs (69+).
    // These must match the constants in ClipboardAtoms.hpp.
    static const struct { const char* name; uint32_t id; } kExtra[] = {
      { "CLIPBOARD",        x11::atom::kCLIPBOARD },
      { "TARGETS",          x11::atom::kTARGETS },
      { "UTF8_STRING",      x11::atom::kUTF8_STRING },
      { "TIMESTAMP",        x11::atom::kTIMESTAMP },
      { "TEXT",             x11::atom::kTEXT },
      { "MULTIPLE",         x11::atom::kMULTIPLE },
      { "INCR",            x11::atom::kINCR },
      { "WM_PROTOCOLS",     x11::atom::kWM_PROTOCOLS },
      { "WM_DELETE_WINDOW", x11::atom::kWM_DELETE_WINDOW },
    };
    for (const auto& e : kExtra) {
      name_to_atom.emplace(std::string(e.name), e.id);
      atom_to_name.emplace(e.id, std::string(e.name));
    }
    next_atom = x11::atom::kLastPreRegistered + 1;
  }
};

static AtomTableState& state() {
  static AtomTableState s;
  return s;
}

AtomTable& AtomTable::instance() {
  static AtomTable t;
  return t;
}

AtomTable::AtomTable() {
  // Nothing; state() handles initialization.
}

uint32_t AtomTable::intern(const char* name, std::size_t len, bool onlyIfExists) {
  if (!name) { name = ""; len = 0; }

  auto& s = state();
  std::lock_guard<std::mutex> lock(s.mu);

  std::string key(name, len);
  auto it = s.name_to_atom.find(key);
  if (it != s.name_to_atom.end()) return it->second;

  if (onlyIfExists) return 0;

  uint32_t atom = s.next_atom++;
  s.name_to_atom.emplace(key, atom);
  s.atom_to_name.emplace(atom, key);
  return atom;
}

const char* AtomTable::name(uint32_t atom, uint32_t* outLen) {
  if (outLen) *outLen = 0;

  // Predefined
  if (atom >= 1 && atom <= 68) {
    const char* s = kPredef[atom - 1];
    if (outLen) *outLen = (uint32_t)std::strlen(s);
    return s;
  }

  auto& st = state();
  std::lock_guard<std::mutex> lock(st.mu);

  auto it = st.atom_to_name.find(atom);
  if (it == st.atom_to_name.end()) {
    // Unknown => empty string
    return "";
  }

  tls_scratch = it->second;
  if (outLen) *outLen = (uint32_t)tls_scratch.size();
  return tls_scratch.c_str();
}

} // namespace x11
