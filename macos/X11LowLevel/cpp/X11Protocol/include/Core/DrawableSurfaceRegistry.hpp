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
//  or freed.
//
//  Synchronization
//  ---------------
//  std::shared_mutex.  Multiple readers proceed in parallel; reallocation
//  takes the exclusive lock briefly.  Read access is exposed via a
//  move-only RAII handle (ReadHandle) that keeps the shared lock held
//  for the duration of the caller's draw op.  ReadHandle is embedded
//  inside DrawableRW (C2), so every draw op naturally holds the lock.
//
//  Buffer ownership (C3, v1.19.35.48-dbg)
//  --------------------------------------
//  The registry now OWNS its host-pixel buffers via std::unique_ptr<HostSurface>.
//  C++ allocates and frees them; Swift never holds a reference.  This
//  closes the ARC-vs-mutex side-channel race that earlier versions had,
//  where Swift's Foundation.Data could be freed by ARC at any moment
//  regardless of what locks the registry held.  Now the only thread that
//  can free a buffer is whichever currently holds the exclusive lock at
//  ensure()/clear() time, after all readers have released.
//
//  Legacy `set(SurfaceDesc)` API: kept transitionally, copies bytes from
//  the externally-provided ptr into a freshly-allocated owned HostSurface.
//  C3.2 will switch Swift to call ensure() directly and the legacy API
//  will be deprecated.
//

#pragma once
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include "Core/SurfaceDesc.hpp"
#include "Core/HostSurface.hpp"

namespace x11 {

class DrawableSurfaceRegistry {
public:
  DrawableSurfaceRegistry() = default;
  ~DrawableSurfaceRegistry() = default;

  DrawableSurfaceRegistry(const DrawableSurfaceRegistry&) = delete;
  DrawableSurfaceRegistry& operator=(const DrawableSurfaceRegistry&) = delete;

  // ── New (C3) ownership API ────────────────────────────────────────────
  // Allocate (or reallocate) the host buffer for `xid` to (w × h × bpr).
  // C++ owns the buffer for its lifetime.  bpr should already include
  // 64-byte stride alignment (caller's responsibility — typically rounded
  // up by the surface bridge from `w*4`).
  //
  // Behaviour:
  //   - If the existing entry already has identical (w, h, bytesPerRow),
  //     this is a no-op and returns the existing generation.  Pixel
  //     contents are NOT touched in the no-op case.
  //   - Otherwise allocates a new HostSurface with posix_memalign(64,...),
  //     fills the new buffer with `fillByte` (0xFF for white, 0x00 for
  //     transparent — caller's choice), drops any previous entry (its
  //     dtor frees the old buffer), inserts the new one, and returns
  //     the new generation.
  //   - Returns 0 on allocation failure.
  //
  // Takes the exclusive lock for the duration of the call.  Blocks until
  // any in-flight ReadHandle releases.  After return, the registry's
  // entry is the new HostSurface and any prior buffer has been freed.
  uint32_t ensure(uint32_t xid, uint16_t w, uint16_t h,
                  uint32_t bytesPerRow, uint8_t fillByte);

  // ── Legacy (transitional) snapshot API ────────────────────────────────
  // set() now allocates an owned HostSurface internally and copies bytes
  // from `s.ptr` into the new buffer.  The externally-provided pointer is
  // not retained — it can be freed immediately after set() returns.
  // C3.2 will replace callers with direct ensure() calls.
  void set(uint32_t xid, const SurfaceDesc& s);

  // Free the buffer for `xid`.  Takes exclusive lock; blocks until any
  // ReadHandle releases.
  void clear(uint32_t xid);

  // Snapshot accessors (read into caller-provided value).
  bool get(uint32_t xid, SurfaceDesc& out) const;
  bool has(uint32_t xid) const;

  // ── RAII read API ─────────────────────────────────────────────────────
  // ReadHandle holds the registry's shared lock for its lifetime.
  // Callers who need to access the buffer should:
  //
  //     auto h = registry.acquireRead(xid);
  //     if (!h.valid()) return;
  //     // ... write/read pixels via h.ptr() / h.bytesPerRow() / h.w() / h.h() ...
  //     // h goes out of scope -> shared lock released
  //
  // While any ReadHandle is alive, the registry's shared_mutex is
  // read-locked; ensure()/clear() blocks until all read handles drop.
  // ReadHandle is embedded inside DrawableRW so every draw op naturally
  // holds the lock.
  //
  //  Re-entrant reads (v1.19.36.34-dbg): only the OUTERMOST acquireRead on a
  //  given thread takes the real shared_lock; nested acquireRead calls on the
  //  same thread borrow it (empty lock_, but still active_ for depth
  //  bookkeeping).  libc++'s shared_mutex is writer-priority: once a writer
  //  (ensure()/clear() during a resize) is pending, a *nested* lock_shared
  //  would block forever while the writer waits for the outer read to release
  //  — a deadlock.  This was hit by `xcalc -rpn` resize, where a fill op holds
  //  the host read lock and re-enters resolveDrawableRW for sibling fills.
  //  Correctness relies on LIFO handle lifetimes, which the stack-scoped
  //  DrawableRW usage guarantees (inner op's handle destroyed before outer's).
  //
  class ReadHandle {
   public:
    ReadHandle() = default;
    ReadHandle(ReadHandle&& o) noexcept
      : lock_(std::move(o.lock_)), desc_(o.desc_), active_(o.active_) {
      o.active_ = false;
      o.desc_ = SurfaceDesc{};
    }
    ReadHandle& operator=(ReadHandle&& o) noexcept {
      if (this != &o) {
        releaseCount_();                 // release our own recursion count first
        lock_   = std::move(o.lock_);
        desc_   = o.desc_;
        active_ = o.active_;
        o.active_ = false;
        o.desc_ = SurfaceDesc{};
      }
      return *this;
    }
    ReadHandle(const ReadHandle&) = delete;
    ReadHandle& operator=(const ReadHandle&) = delete;
    ~ReadHandle() { releaseCount_(); }

    bool valid() const { return desc_.ptr != nullptr; }
    const SurfaceDesc& desc() const { return desc_; }
    void*    ptr()         const { return desc_.ptr; }
    uint32_t bytesPerRow() const { return desc_.bytesPerRow; }
    uint16_t w()           const { return desc_.w; }
    uint16_t h()           const { return desc_.h; }
    uint32_t generation()  const { return desc_.generation; }

   private:
    friend class DrawableSurfaceRegistry;
    ReadHandle(std::shared_lock<std::shared_mutex>&& l, SurfaceDesc d, bool active)
      : lock_(std::move(l)), desc_(d), active_(active) {}
    // Decrement this thread's read-recursion depth exactly once per active
    // handle.  Out-of-line (in the .cpp) so it can reach the thread_local
    // depth after the enclosing class is complete.  The outermost handle also
    // holds the real shared_lock in lock_, which auto-unlocks after this runs.
    void releaseCount_();
    std::shared_lock<std::shared_mutex> lock_;
    SurfaceDesc desc_ {};
    bool active_ = false;   // participates in the thread's read-depth count
  };

  ReadHandle acquireRead(uint32_t xid) const;

private:
  // Build a SurfaceDesc snapshot pointing at the underlying HostSurface
  // buffer.  Caller must hold either lock when calling.
  static SurfaceDesc snapshotFromSurface(const HostSurface& s);

  mutable std::shared_mutex mu_;
  std::unordered_map<uint32_t, std::unique_ptr<HostSurface>> map_;

  // Per-thread nesting depth for read locks (see ReadHandle).  Only depth 0→1
  // takes the real shared_lock; nested acquireRead on the same thread borrows
  // it.  Defined in the .cpp.
  static thread_local int t_readDepth_;
};

} // namespace x11
