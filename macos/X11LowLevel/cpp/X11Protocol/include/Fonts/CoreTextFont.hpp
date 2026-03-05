//
//  CoreTextFont.hpp
//  X11LowLevel
//
//  CoreText bridge: maps X11 font requests to macOS system fonts.
//  Rasterizes glyphs via CoreText/CoreGraphics C API (no Swift needed).
//  Supports both 1-bit crisp and 8-bit antialiased rendering.
//

#pragma once
#include <memory>
#include <string>
#include <atomic>

#include "Fonts/BDF.hpp"

namespace x11::font {

/// Runtime toggle for antialiased font rendering.
/// When true, text ops use the alpha channel (smooth edges).
/// When false, text ops use the 1-bit bitmap (crisp/bitmap style).
bool antialiasedFonts();
void setAntialiasedFonts(bool enabled);

/// Create a BdfFont from macOS CoreText, rasterizing Latin-1 glyphs (0-255).
/// Each glyph gets both a 1-bit bitmap and an 8-bit alpha coverage map.
///
/// @param xlfdOrName  Full XLFD string (e.g. "-*-helvetica-bold-r-*--14-*")
///                    or bare font name (e.g. "Menlo", "courier").
/// @param pixelSize   Desired pixel size (used when XLFD field 7 is 0 or absent).
/// @return            Populated BdfFont, or nullptr if CoreText can't resolve the font.
std::unique_ptr<BdfFont> createCoreTextFont(const std::string& xlfdOrName, int pixelSize);

} // namespace x11::font
