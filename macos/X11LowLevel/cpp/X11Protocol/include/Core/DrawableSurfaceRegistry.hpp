//
//  DrawableSurfaceRegistry.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/22/26.
//
//  Per-host surface registry, lookup-keyed by top-level X11 window XID.
//
//  Concurrency
//  -----------
//  Three classes of access (now properly synchronized):
//
//    1. xproto thread WRITES pixels via resolveDrawableRW (many per frame).
//    2. xproto thread READS pixels via CopyArea source (occasional).
//    3. Swift main thread READS pixels for Metal upload (per present tick).
//
//  Plus, *exclusive* writers when the surface is allocated, reallocated,
//  or freed.  Old design: a plain std::mutex protected only the map
//  structure; the buffers themselves were owned by Swift's
//  Foundation.Data with no cross-thread guarantee (ARC could free a
//  buffer mid-write).  ASan caught that race in v1.19.35.45-dbg
//  (heap-use-after-free in fillWindowBackgroundIfReady during live
//  resize).
//
//  New design: std::shared_mutex.  Multiple readers proceed in parallel;
//  reallocation takes the exclusive lock briefly.  Read access is
//  exposed via a move-only RAII handle (ReadHandle) that keeps the
//  shared lock held for the duration of the caller's draw op.
//  Subsequent commits (C2, C3) will move buffer ownership from Swift to
//  C++ (HostSurface), embed ReadHandle inside DrawableRW, and route
//  reallocation through a host command queue — see docs/TODO.md for the
//  full plan.
//

#pragma once
#include <cstdint>
#include <unordered_map>
#include <shared_mutex>
#include "Core/SurfaceDesc.hpp"

namespace x11 {

class DrawableSurfaceRegistry {
public:
  DrawableSurfaceRegistry() = default;
  ~DrawableSurfaceRegistry() = default;

  DrawableSurfaceRegistry(const DrawableSurfaceRegistry&) = delete;
  DrawableSurfaceRegistry& operator=(const DrawableSurfaceRegistry&) = delete;

  // ── Existing (legacy) snapshot API ────────────────────────────────────
  // These continue to work after C1; callers receive a value-copied
  // SurfaceDesc.  The buffer pointer in the returned descriptor is valid
  // only as long as no one reallocates (so on the xproto thread, between
  // host commands, it's stable in practice — but C2 will switch the
  // hot-path callers (resolveDrawableRW) to acquireRead() for explicit
  // lifetime safety).
  void set(uint32_t xid, const SurfaceDesc& s);
  void clear(uint32_t xid);
  bool get(uint32_t xid, SurfaceDesc& out) const;
  bool has(uint32_t xid) const;

  // ── New (C1) RAII read API ────────────────────────────────────────────
  // ReadHandle holds the registry's shared lock for its lifetime.
  // Callers who need to access the buffer should:
  //
  //     auto h = registry.acquireRead(xid);
  //     if (!h.valid()) return;
  //     // ... write/read pixels via h.ptr() / h.bytesPerRow() / h.w() / h.h() ...
  //     // h goes out of scope -> shared lock released
  //
  // While any ReadHandle is alive, the registry's shared_mutex is
  // read-locked; reallocation/clear blocks until all read handles drop.
  // Designed to be embedded inside DrawableRW in commit C2 so every
  // draw op naturally holds the lock for its duration.
  class ReadHandle {
   public:
    ReadHandle() = default;
    ReadHandle(ReadHandle&&) noexcept = default;
    ReadHandle& operator=(ReadHandle&&) noexcept = default;
    ReadHandle(const ReadHandle&) = delete;
    ReadHandle& operator=(const ReadHandle&) = delete;
    ~ReadHandle() = default;

    bool valid() const { return desc_.ptr != nullptr; }
    const SurfaceDesc& desc() const { return desc_; }
    void*    ptr()         const { return desc_.ptr; }
    uint32_t bytesPerRow() const { return desc_.bytesPerRow; }
    uint16_t w()           const { return desc_.w; }
    uint16_t h()           const { return desc_.h; }
    uint32_t generation()  const { return desc_.generation; }

   private:
    friend class DrawableSurfaceRegistry;
    ReadHandle(std::shared_lock<std::shared_mutex>&& l, SurfaceDesc d)
      : lock_(std::move(l)), desc_(d) {}
    std::shared_lock<std::shared_mutex> lock_;
    SurfaceDesc desc_ {};
  };

  // Acquire a read handle for `xid`.  If no surface is registered (or
  // the entry has a null pointer), returns an invalid handle (valid() ==
  // false) with no lock held.
  ReadHandle acquireRead(uint32_t xid) const;

private:
  mutable std::shared_mutex mu_;
  std::unordered_map<uint32_t, SurfaceDesc> map_;
};

} // namespace x11
