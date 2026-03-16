//
//  XClient.cpp
//  SwiftX11
//

#include "Core/XClient.hpp"

namespace x11 {

XClient::XClient(XProtoContext& ctx, EventOps& evOps,
                 int fd, uint32_t rid_base, uint32_t rid_mask)
  : fd_(fd)
  , rid_base_(rid_base)
  , rid_mask_(rid_mask)
  , transport_(ctx, evOps)
  , reply_(transport_)
{
  transport_.attachClientFd(fd);
  transport_.setClientIdSpace(rid_base, rid_mask);
  transport_.setXprotoThreadSelf();
}

XClient::~XClient() = default;

// ---------------------------------------------------------------------------
// XC-MISC XID allocation
// ---------------------------------------------------------------------------
// Xlib allocates XIDs from the bottom of the client's range (1, 2, 3…).
// We allocate from the midpoint upward so there's no collision until the
// client has used ~8M XIDs (half of the 16M range), which is unrealistic.

std::pair<uint32_t, uint32_t>
XClient::allocXIDRange(uint32_t requested) {
  if (xid_alloc_cursor_ == 0)
    xid_alloc_cursor_ = (rid_mask_ / 2) + 1;  // start at midpoint

  const uint32_t remaining = rid_mask_ - xid_alloc_cursor_ + 1;
  if (remaining == 0)
    return {0, 0};  // exhausted

  const uint32_t count = std::min(requested, remaining);
  const uint32_t start = rid_base_ | xid_alloc_cursor_;
  xid_alloc_cursor_ += count;
  return {start, count};
}

uint32_t
XClient::allocXIDList(uint32_t* out, uint32_t count) {
  if (xid_alloc_cursor_ == 0)
    xid_alloc_cursor_ = (rid_mask_ / 2) + 1;

  uint32_t allocated = 0;
  for (uint32_t i = 0; i < count && xid_alloc_cursor_ <= rid_mask_; ++i) {
    out[i] = rid_base_ | xid_alloc_cursor_++;
    ++allocated;
  }
  return allocated;
}

} // namespace x11
