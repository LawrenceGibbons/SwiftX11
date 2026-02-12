//
//  CursorTable.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/10/26.
//

#include "CursorTable.hpp"

namespace x11 {

void CursorTable::createCursorPixmap(uint32_t cid,
                                     uint32_t srcPixmap, uint32_t maskPixmap,
                                     uint16_t hotX, uint16_t hotY,
                                     RGB16 fg, RGB16 bg)
{
  if (cid == 0) return;
  std::lock_guard<std::mutex> lk(mu_);
  Cursor c{};
  c.cid = cid;
  c.kind = Cursor::Kind::Pixmap;
  c.source_pixmap = srcPixmap;
  c.mask_pixmap   = maskPixmap;
  c.hot_x = hotX;
  c.hot_y = hotY;
  c.fg = fg;
  c.bg = bg;
  map_[cid] = c;
}

void CursorTable::createCursorGlyph(uint32_t cid,
                                    uint32_t srcFont, uint32_t maskFont,
                                    uint16_t srcChar, uint16_t maskChar,
                                    RGB16 fg, RGB16 bg)
{
  if (cid == 0) return;
  std::lock_guard<std::mutex> lk(mu_);
  Cursor c{};
  c.cid = cid;
  c.kind = Cursor::Kind::Glyph;
  c.source_font = srcFont;
  c.mask_font   = maskFont;
  c.source_char = srcChar;
  c.mask_char   = maskChar;
  c.fg = fg;
  c.bg = bg;
  map_[cid] = c;
}

bool CursorTable::exists(uint32_t cid) const {
  if (cid == 0) return false;
  std::lock_guard<std::mutex> lk(mu_);
  return map_.find(cid) != map_.end();
}

bool CursorTable::erase(uint32_t cid) {
  if (cid == 0) return false;
  std::lock_guard<std::mutex> lk(mu_);
  return map_.erase(cid) != 0;
}

bool CursorTable::recolor(uint32_t cid, RGB16 fg, RGB16 bg) {
  if (cid == 0) return false;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = map_.find(cid);
  if (it == map_.end()) return false;
  it->second.fg = fg;
  it->second.bg = bg;
  return true;
}

bool CursorTable::get(uint32_t cid, Cursor& out) const {
  if (cid == 0) return false;
  std::lock_guard<std::mutex> lk(mu_);
  auto it = map_.find(cid);
  if (it == map_.end()) return false;
  out = it->second;
  return true;
}

} // namespace x11
