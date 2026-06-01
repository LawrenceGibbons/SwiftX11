//
//  HostSurface.hpp
//  X11LowLevel
//
//  Owned host (top-level X11 window) pixel buffer.  Allocated and freed
//  exclusively by C++ — Swift no longer holds Foundation.Data references.
//
//  Lifetime
//  --------
//  HostSurface is owned by DrawableSurfaceRegistry via std::unique_ptr.
//  Its existence is bracketed by ensure()/clear() calls on the registry,
//  which take the registry's exclusive lock.  As long as a HostSurface is
//  in the registry's map, its buffer pointer is stable — no concurrent
//  realloc can free it because:
//    - any in-flight reader holds the shared lock and blocks the writer;
//    - the writer (ensure() / clear()) holds the exclusive lock and is
//      blocked by readers.
//  The only thread that frees the buffer is whichever holds the exclusive
//  lock at the moment of replacement, after all readers have released.
//
//  This eliminates the ARC-vs-shared_mutex side-channel race that plagued
//  earlier versions where the buffer was a Swift Foundation.Data owned by
//  X11View.hostSurface — Swift's ARC could free that buffer at any time
//  regardless of what locks the C++ registry held, because the lock and
//  the Data's reference count were independent.  C3 closes that gap by
//  removing Swift's ability to own or free the buffer at all.
//
//  Allocation
//  ----------
//  posix_memalign with 64-byte alignment so the row stride (also rounded
//  up to a multiple of 64 bytes) lands on a cache line boundary.  This
//  matches what C++ draw ops already assume via DrawableRW::stridePixels.
//

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>

namespace x11 {

struct HostSurface {
  void*    ptr           = nullptr;   // posix_memalign(64, capacityBytes)
  size_t   capacityBytes = 0;          // backing allocation size
  uint16_t w             = 0;
  uint16_t h             = 0;
  uint32_t bytesPerRow   = 0;          // 64-byte aligned for the live (w,h)
  uint32_t generation    = 0;          // bumped on each successful realloc

  HostSurface() = default;

  ~HostSurface() {
    if (ptr) {
      ::free(ptr);
      ptr = nullptr;
    }
  }

  HostSurface(const HostSurface&)            = delete;
  HostSurface& operator=(const HostSurface&) = delete;
  HostSurface(HostSurface&&)                 = delete;
  HostSurface& operator=(HostSurface&&)      = delete;
};

} // namespace x11
