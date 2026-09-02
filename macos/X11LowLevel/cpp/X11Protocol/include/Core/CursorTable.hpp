//
//  CursorTable.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/10/26.
//

#pragma once

#include <cstdint>
#include <unordered_map>
#include <mutex>

namespace x11 {

// Minimal cursor model for bring-up.
// We don't render cursors yet; we just track resources so clients don't trip protocol.
class CursorTable {
public:
  struct RGB16 { uint16_t r=0, g=0, b=0; };

  struct Cursor {
    uint32_t cid = 0;

    // CreateCursor (pixmap) form
    uint32_t source_pixmap = 0;
    uint32_t mask_pixmap   = 0;
    uint16_t hot_x = 0;
    uint16_t hot_y = 0;

    // CreateGlyphCursor (font glyph) form
    uint32_t source_font = 0;
    uint32_t mask_font   = 0;
    uint16_t source_char = 0;
    uint16_t mask_char   = 0;

    // Colors
    RGB16 fg{};
    RGB16 bg{};

    enum class Kind : uint8_t { Pixmap, Glyph } kind = Kind::Pixmap;
  };

  // Upsert/overwrite.
  void createCursorPixmap(uint32_t cid,
                          uint32_t srcPixmap, uint32_t maskPixmap,
                          uint16_t hotX, uint16_t hotY,
                          RGB16 fg, RGB16 bg);

  void createCursorGlyph(uint32_t cid,
                         uint32_t srcFont, uint32_t maskFont,
                         uint16_t srcChar, uint16_t maskChar,
                         RGB16 fg, RGB16 bg);

  bool exists(uint32_t cid) const;

  // Returns true if removed.
  bool erase(uint32_t cid);

  // Free all cursors owned by a disconnecting client (XID range).
  // Review §6.4 — cursors leaked on every client disconnect.
  size_t eraseOwnedBy(uint32_t clientBase, uint32_t clientMask);

  // Returns false if not found.
  bool recolor(uint32_t cid, RGB16 fg, RGB16 bg);

  // Optional: snapshot for debugging
  bool get(uint32_t cid, Cursor& out) const;

private:
  mutable std::mutex mu_;
  std::unordered_map<uint32_t, Cursor> map_;
};

} // namespace x11
