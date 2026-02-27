#pragma once
#include <cstdint>
#include <cstddef>

namespace x11 {
bool lookupColorName(const char* name, std::size_t nameLen,
                     uint8_t& r, uint8_t& g, uint8_t& b);
} // namespace x11
