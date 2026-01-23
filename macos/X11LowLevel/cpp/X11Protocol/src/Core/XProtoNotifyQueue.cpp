//
//  XProtoNotifyQueue.cpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "XProtoNotifyQueue.hpp"
#include <cstddef>

namespace x11 {

static inline void union_rect(
  uint16_t& x, uint16_t& y, uint16_t& w, uint16_t& h,
  uint16_t nx, uint16_t ny, uint16_t nw, uint16_t nh)
{
  if (w == 0 || h == 0) { x=nx; y=ny; w=nw; h=nh; return; }
  uint32_t x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  uint32_t nx0 = nx, ny0 = ny, nx1 = nx + nw, ny1 = ny + nh;
  uint32_t ux0 = (x0 < nx0) ? x0 : nx0;
  uint32_t uy0 = (y0 < ny0) ? y0 : ny0;
  uint32_t ux1 = (x1 > nx1) ? x1 : nx1;
  uint32_t uy1 = (y1 > ny1) ? y1 : ny1;
  x = (uint16_t)ux0; 
  y = (uint16_t)uy0;
  uint32_t uw = ux1 - ux0;
  uint32_t uh = uy1 - uy0;
  if (uw > 0xFFFFu) uw = 0xFFFFu;
  if (uh > 0xFFFFu) uh = 0xFFFFu;
  w = (uint16_t)uw;
  h = (uint16_t)uh;
}
  
  
XProtoNotifyQueue::XProtoNotifyQueue() {
  pthread_mutex_init(&mu_, nullptr);
  // pending_ is POD; no need to memset, but ok either way.
}

XProtoNotifyQueue::~XProtoNotifyQueue() {
  pthread_mutex_destroy(&mu_);
}

  
  void XProtoNotifyQueue::queueExposeRect(uint32_t wid,
                                          uint16_t x, uint16_t y,
                                          uint16_t w, uint16_t h,
                                          uint16_t count)
{
  if (wid == 0) return;
  if (w == 0 || h == 0) return;

  pthread_mutex_lock(&mu_);

  // Try to coalesce into existing entry for wid
  for (size_t i = 0; i < pending_n_; i++) {
    if (pending_[i].wid == wid) {
      // If already full-window expose, do NOT downgrade to rect
      if (!pending_[i].expose_has_rect) {
        pending_[i].want_expose = 1;
        pthread_mutex_unlock(&mu_);
        return;
      }

      pending_[i].want_expose = 1;
      pending_[i].expose_has_rect = 1;

      // union rect (or “last write wins” if you want simplest first)
      union_rect(pending_[i].expose_x,
                 pending_[i].expose_y,
                 pending_[i].expose_w,
                 pending_[i].expose_h,
                 x, y, w, h);

      pending_[i].expose_count = count; // ok: last wins
      pthread_mutex_unlock(&mu_);
      return;
    }
  }

  // Add new
  if (pending_n_ < kMaxPending) {
    PendingNotify& pn = pending_[pending_n_++];
    pn.wid = wid;
    pn.want_configure = 0;
    pn.want_expose = 1;

    pn.expose_has_rect = 1;
    pn.expose_x = x;
    pn.expose_y = y;
    pn.expose_w = w;
    pn.expose_h = h;
    pn.expose_count = count;
  }

  pthread_mutex_unlock(&mu_);
}
  
  
void XProtoNotifyQueue::coalesceLocked(uint32_t wid, bool wantConfigure, bool wantExpose) {
  // Coalesce by wid (same logic as C)
  for (size_t i = 0; i < pending_n_; i++) {
    if (pending_[i].wid == wid) {
      if (wantConfigure) pending_[i].want_configure = 1;
      if (wantExpose) {
        pending_[i].want_expose = 1;

        // IMPORTANT: a “full expose” dominates any prior rect expose.
        pending_[i].expose_has_rect = 0;
        pending_[i].expose_x = pending_[i].expose_y = 0;
        pending_[i].expose_w = pending_[i].expose_h = 0;
        pending_[i].expose_count = 0;
      }
      return;
    }
  }

  if (pending_n_ < kMaxPending) {
    PendingNotify& pn = pending_[pending_n_++];
    pn.wid = wid;
    pn.want_configure = wantConfigure ? 1 : 0;
    pn.want_expose    = wantExpose ? 1 : 0;

    pn.expose_has_rect = 0;
    pn.expose_x = pn.expose_y = pn.expose_w = pn.expose_h = 0;
    pn.expose_count = 0;
  }
}

void XProtoNotifyQueue::queueNotify(uint32_t wid, bool wantConfigure, bool wantExpose) {
  if (wid == 0) return;
  if (!wantConfigure && !wantExpose) return;

  pthread_mutex_lock(&mu_);
  coalesceLocked(wid, wantConfigure, wantExpose);
  pthread_mutex_unlock(&mu_);
}

size_t XProtoNotifyQueue::drain(PendingNotify* out, size_t max) {
  if (!out || max == 0) return 0;

  pthread_mutex_lock(&mu_);
  size_t n = pending_n_;
  if (n > max) n = max;

  // Copy out
  for (size_t i = 0; i < n; i++) out[i] = pending_[i];

  // Compact remaining (exactly like your C)
  if (n == pending_n_) {
    pending_n_ = 0;
  } else {
    const size_t remain = pending_n_ - n;
    for (size_t i = 0; i < remain; i++) {
      pending_[i] = pending_[n + i];
    }
    pending_n_ = remain;
  }

  pthread_mutex_unlock(&mu_);
  return n;
}

} // namespace x11
