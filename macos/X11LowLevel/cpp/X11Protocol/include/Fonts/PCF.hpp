//
//  PCF.hpp
//  X11LowLevel
//
//  PCF (Portable Compiled Font) parser.  Reads .pcf.gz files from the
//  X11 font directories and converts them into BdfFont structs that the
//  rest of the server already understands.
//

#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "Fonts/BDF.hpp"

namespace x11::font {

/// Parse a PCF font from raw (decompressed) bytes.
/// On success, populates `out` with metrics, glyph bitmaps, and the XLFD name.
/// Returns a BdfParseResult with ok=true on success.
BdfParseResult parsePcf(const uint8_t* data, size_t len, BdfFont& out);

/// Load a .pcf.gz file from disk, decompress it, and parse it.
/// Returns a BdfParseResult.
BdfParseResult loadPcfGz(const std::string& path, BdfFont& out);

} // namespace x11::font
