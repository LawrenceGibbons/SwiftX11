//
//  AtomTable.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/24/26.
//

// AtomTable.hpp
#pragma once

#include <cstdint>
#include <cstddef>

namespace x11 {

class AtomTable {
public:
  static AtomTable& instance();

  // Intern a name (len bytes, not necessarily null-terminated).
  // If onlyIfExists=true, returns 0 if not already present.
  uint32_t intern(const char* name, std::size_t len, bool onlyIfExists);

  // Returns pointer to a stable C string for this thread until next name() call.
  // outLen gets length in bytes (no trailing null).
  const char* name(uint32_t atom, uint32_t* outLen);

private:
  AtomTable();
  AtomTable(const AtomTable&) = delete;
  AtomTable& operator=(const AtomTable&) = delete;
};

} // namespace x11

