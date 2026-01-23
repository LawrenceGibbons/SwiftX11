//
//  XProtoNotifyQueue.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once
#include <cstdint>
#include <cstddef>
#include <pthread.h>
 
#include "XProtoPendingNotify.hpp"

namespace x11 {

class XProtoNotifyQueue {
public:
  XProtoNotifyQueue();
  ~XProtoNotifyQueue();

  // Thread-safe: can be called from any thread.
  void queueNotify(uint32_t wid, bool wantConfigure, bool wantExpose);

  void queueExposeRect(uint32_t wid,
                       uint16_t x, uint16_t y,
                       uint16_t w, uint16_t h,
                       uint16_t count);
  
  // Must be called on xproto thread.
  // Moves up to max items into out[], returns number moved.
  size_t drain(PendingNotify* out, size_t max);

private:
  static constexpr size_t kMaxPending = 1024;

  pthread_mutex_t mu_;
  PendingNotify pending_[kMaxPending];
  size_t pending_n_ = 0;

  void coalesceLocked(uint32_t wid, bool wantConfigure, bool wantExpose);
};

} // namespace x11
