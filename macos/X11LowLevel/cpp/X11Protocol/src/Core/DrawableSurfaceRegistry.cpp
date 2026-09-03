//
//  DrawableSurfaceRegistry.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/22/26.
//

#include "Core/DrawableSurfaceRegistry.hpp"

#include <cstdlib>   // posix_memalign, free
#include <cstring>   // memcpy

namespace x11 {

// Per-thread read-lock recursion depth (see DrawableSurfaceRegistry::ReadHandle).
thread_local int DrawableSurfaceRegistry::t_readDepth_ = 0;

void DrawableSurfaceRegistry::ReadHandle::releaseCount_() {
  if (active_) {
    if (t_readDepth_ > 0) --t_readDepth_;
    active_ = false;
  }
  // lock_ (real shared_lock only for the outermost handle) auto-unlocks when
  // this handle's members destruct, i.e. after this function returns.
}

// ── Internal helpers ────────────────────────────────────────────────────

SurfaceDesc DrawableSurfaceRegistry::snapshotFromSurface(const HostSurface& s) {
  SurfaceDesc d{};
  d.ptr         = s.ptr;
  d.bytesPerRow = s.bytesPerRow;
  d.w           = s.w;
  d.h           = s.h;
  d.format      = SurfaceFormat::XRGB8888;
  d.generation  = s.generation;
  return d;
}

// ── New (C3) ownership API ──────────────────────────────────────────────

uint32_t DrawableSurfaceRegistry::ensure(uint32_t xid,
                                         uint16_t w,
                                         uint16_t h,
                                         uint32_t bytesPerRow,
                                         uint8_t  fillByte)
{
  if (xid == 0 || w == 0 || h == 0 || bytesPerRow == 0) return 0;

  // 64-byte alignment for the row stride (matches existing draw-op
  // assumptions).  Caller usually pre-aligns; we just sanity-check.
  if ((bytesPerRow & 63u) != 0) {
    // Round up internally; preserves old caller behaviour without erroring.
    bytesPerRow = (bytesPerRow + 63u) & ~63u;
  }

  std::unique_lock<std::shared_mutex> lock(mu_);

  // No-op if same shape already there.  We deliberately do NOT touch
  // pixel contents in the no-op case — caller may have meaningful pixels
  // that should survive a redundant ensure() call (e.g., backing store).
  auto it = map_.find(xid);
  if (it != map_.end() && it->second &&
      it->second->w == w && it->second->h == h &&
      it->second->bytesPerRow == bytesPerRow)
  {
    return it->second->generation;
  }

  const size_t needBytes = (size_t)bytesPerRow * (size_t)h;
  void* mem = nullptr;
  // posix_memalign requires alignment to be a power of two and a multiple
  // of sizeof(void*).  64 satisfies both on arm64.
  if (::posix_memalign(&mem, 64, needBytes) != 0 || !mem) {
    return 0;
  }

  // Fill the freshly-allocated buffer.  Caller passes the byte to splat
  // across all pixels: 0xFF for white BGRA, 0x00 for transparent.
  std::memset(mem, fillByte, needBytes);

  auto surf = std::make_unique<HostSurface>();
  surf->ptr           = mem;
  surf->capacityBytes = needBytes;
  surf->w             = w;
  surf->h             = h;
  surf->bytesPerRow   = bytesPerRow;
  // Bump generation across realloc so consumers can detect change.
  if (it != map_.end() && it->second) {
    surf->generation = it->second->generation + 1;
  } else {
    surf->generation = 1;
  }

  map_[xid] = std::move(surf);
  return map_[xid]->generation;
}

// ── Legacy (transitional) snapshot API ──────────────────────────────────

void DrawableSurfaceRegistry::set(uint32_t xid, const SurfaceDesc& s) {
  if (xid == 0 || !s.ptr || s.w == 0 || s.h == 0 || s.bytesPerRow == 0) return;

  // Allocate (or grow) the owned buffer to the desired shape, then copy
  // bytes in.  This preserves the legacy "Swift owns buffer, calls set()
  // to publish ptr" call pattern transitionally — but C++ now owns its
  // own copy so Swift can free its Foundation.Data immediately after set()
  // returns.  C3.2 will switch Swift to call ensure() directly and skip
  // this memcpy.  Fill byte is irrelevant since we'll overwrite with the
  // memcpy below; pass 0 to keep behaviour deterministic.
  const uint32_t gen = ensure(xid, s.w, s.h, s.bytesPerRow, /*fillByte=*/0);
  if (gen == 0) return;

  std::unique_lock<std::shared_mutex> lock(mu_);
  auto it = map_.find(xid);
  if (it == map_.end() || !it->second || !it->second->ptr) return;

  HostSurface& dst = *it->second;
  // Copy row by row in case bytesPerRow > w*4 with right-edge padding.
  // (Most callers use bpr == w*4, but we defensively support strided sources.)
  const size_t copyBytesPerRow = (size_t)s.w * 4u;
  const size_t srcStride = s.bytesPerRow;
  const size_t dstStride = dst.bytesPerRow;
  const uint8_t* src = static_cast<const uint8_t*>(s.ptr);
  uint8_t* dstP      = static_cast<uint8_t*>(dst.ptr);
  for (uint32_t y = 0; y < s.h; ++y) {
    std::memcpy(dstP + (size_t)y * dstStride,
                src  + (size_t)y * srcStride,
                copyBytesPerRow);
  }
}

void DrawableSurfaceRegistry::clear(uint32_t xid) {
  if (xid == 0) return;
  std::unique_lock<std::shared_mutex> lock(mu_);
  map_.erase(xid);  // unique_ptr dtor frees buffer
}

bool DrawableSurfaceRegistry::get(uint32_t xid, SurfaceDesc& out) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto it = map_.find(xid);
  if (it == map_.end() || !it->second || !it->second->ptr) return false;
  out = snapshotFromSurface(*it->second);
  return true;
}

bool DrawableSurfaceRegistry::has(uint32_t xid) const {
  std::shared_lock<std::shared_mutex> lock(mu_);
  auto it = map_.find(xid);
  return it != map_.end() && it->second && it->second->ptr != nullptr;
}

// ── RAII read API ───────────────────────────────────────────────────────

DrawableSurfaceRegistry::ReadHandle
DrawableSurfaceRegistry::acquireRead(uint32_t xid) const {
  if (xid == 0) return ReadHandle{};   // inactive, does not touch the depth

  // Re-entrant: take the real shared_lock only for the outermost read on this
  // thread.  A nested acquireRead (e.g. a sibling fill re-entering
  // resolveDrawableRW while an outer fill still holds its handle) must NOT
  // lock_shared again — libc++'s writer-priority shared_mutex would deadlock it
  // against a pending resize writer.  The outer handle's lock keeps the buffer
  // stable for the whole nest; the depth counter tracks the recursion.
  const bool outermost = (t_readDepth_ == 0);
  std::shared_lock<std::shared_mutex> lock;              // empty for nested reads
  if (outermost) lock = std::shared_lock<std::shared_mutex>(mu_);
  ++t_readDepth_;

  auto it = map_.find(xid);
  if (it == map_.end() || !it->second || !it->second->ptr) {
    // Still an ACTIVE handle so the depth (and any real lock) is released on
    // destruction; caller sees !valid() and returns.
    return ReadHandle(std::move(lock), SurfaceDesc{}, /*active=*/true);
  }
  return ReadHandle(std::move(lock), snapshotFromSurface(*it->second), /*active=*/true);
}

} // namespace x11
