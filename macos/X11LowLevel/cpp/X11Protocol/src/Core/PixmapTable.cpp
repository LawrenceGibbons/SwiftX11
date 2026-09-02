//
//  PixmapTable.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/28/26.
//

#include <algorithm>
#include <cstring>

#include "Core/PixmapTable.hpp"


namespace x11 {

static inline uint16_t clamp1(uint16_t v) { return (v == 0) ? 1 : v; }

uint32_t PixmapTable::computeStrideBytesDepth1(uint16_t w) {
  // X11 bitmapScanlinePad=32 => scanlines padded to 32 bits.
  // stride_bytes = ((w + 31) & ~31) / 8
  uint32_t ww = (uint32_t)w;
  uint32_t padded = (ww + 31u) & ~31u;
  return padded >> 3;
}

void PixmapTable::createPixmap(uint32_t xid, uint8_t depth, uint16_t w, uint16_t h) {
  if (xid == 0) return;

  w = clamp1(w);
  h = clamp1(h);

  std::lock_guard<std::mutex> lock(mu_);

  PixmapState& st = map_[xid];
  st.xid = xid;
  st.depth = depth;
  st.w = w;
  st.h = h;

  st.bits.clear();
  st.pixels.clear();
  st.stride_bytes = 0;

  if (depth == 1) {
    st.stride_bytes = computeStrideBytesDepth1(w);
    const size_t nbytes = (size_t)st.stride_bytes * (size_t)h;
    st.bits.assign(nbytes, 0); // background/off
  } else {
    const size_t npx = (size_t)w * (size_t)h;
    st.pixels.assign(npx, 0xFFFFFFFFu); // white
  }
}

void PixmapTable::freePixmap(uint32_t xid) {
  if (xid == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  map_.erase(xid);
}

size_t PixmapTable::eraseOwnedBy(uint32_t clientBase, uint32_t clientMask) {
  std::lock_guard<std::mutex> lock(mu_);
  const uint32_t hi = ~clientMask;
  size_t n = 0;
  for (auto it = map_.begin(); it != map_.end(); ) {
    if ((it->first & hi) == (clientBase & hi)) { it = map_.erase(it); ++n; }
    else ++it;
  }
  return n;
}

bool PixmapTable::exists(uint32_t xid) const {
  std::lock_guard<std::mutex> lock(mu_);
  return map_.find(xid) != map_.end();
}

bool PixmapTable::snapshot(uint32_t xid, PixmapView& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = map_.find(xid);
  if (it == map_.end()) return false;

  const PixmapState& st = it->second;
  out.xid = st.xid;
  out.w = st.w;
  out.h = st.h;
  out.depth = st.depth;

  if (st.depth == 1) {
    out.stride_bytes = st.stride_bytes;
    out.bits = st.bits.empty() ? nullptr : st.bits.data();
    out.pixels = nullptr;
  } else {
    out.stride_bytes = 0;
    out.bits = nullptr;
    out.pixels = st.pixels.empty() ? nullptr : st.pixels.data();
  }
  return true;
}

uint32_t* PixmapTable::mutablePixels(uint32_t xid, uint16_t* outW, uint16_t* outH) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = map_.find(xid);
  if (it == map_.end()) return nullptr;

  PixmapState& st = it->second;
  if (st.depth == 1) return nullptr;
  if (outW) *outW = st.w;
  if (outH) *outH = st.h;
  return st.pixels.empty() ? nullptr : st.pixels.data();
}

uint8_t* PixmapTable::mutableBits(uint32_t xid, uint16_t* outW, uint16_t* outH, uint32_t* outStrideBytes) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = map_.find(xid);
  if (it == map_.end()) return nullptr;

  PixmapState& st = it->second;
  if (st.depth != 1) return nullptr;
  if (outW) *outW = st.w;
  if (outH) *outH = st.h;
  if (outStrideBytes) *outStrideBytes = st.stride_bytes;
  return st.bits.empty() ? nullptr : st.bits.data();
}

} // namespace x11
