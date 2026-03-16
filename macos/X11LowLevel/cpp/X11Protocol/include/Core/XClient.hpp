//
//  XClient.hpp
//  SwiftX11
//
//  Per-connection state for an X11 client session.
//  Server-wide state lives in XProtoServer; XClient holds the per-connection
//  transport, reply writer, and client ID space.
//

#pragma once
#include <cstdint>
#include <utility>
#include <Transport/XProtoTransport.hpp>
#include <Ops/ReplyWriter.hpp>

namespace x11 {

class XProtoContext;
class EventOps;

class XClient {
public:
  XClient(XProtoContext& ctx, EventOps& evOps,
          int fd, uint32_t rid_base, uint32_t rid_mask);
  ~XClient();

  XProtoTransport& transport() { return transport_; }
  const XProtoTransport& transport() const { return transport_; }

  ReplyWriter& reply() { return reply_; }
  const ReplyWriter& reply() const { return reply_; }

  int fd() const { return fd_; }
  uint32_t ridBase() const { return rid_base_; }
  uint32_t ridMask() const { return rid_mask_; }

  // BIG-REQUESTS extension: when enabled, len_words==0 means 4 extra bytes follow
  bool bigReqEnabled() const { return big_req_enabled_; }
  void setBigReqEnabled(bool v) { big_req_enabled_ = v; }

  // XC-MISC: allocate a range of XIDs from this client's ID space.
  // Xlib allocates from the bottom (1, 2, 3…); we allocate from the top
  // downward to avoid collision until the client exhausts ~8M XIDs.
  // Returns (start_xid, count).  (0, 0) means exhausted.
  std::pair<uint32_t, uint32_t> allocXIDRange(uint32_t requested);

  // XC-MISC: allocate individual XIDs (up to count).  Returns actual count.
  uint32_t allocXIDList(uint32_t* out, uint32_t count);

private:
  int fd_;
  uint32_t rid_base_;
  uint32_t rid_mask_;
  XProtoTransport transport_;
  ReplyWriter reply_;
  bool big_req_enabled_ = false;
  // XC-MISC: cursor for XID allocation, starts at rid_mask/2 and grows upward
  uint32_t xid_alloc_cursor_ = 0;  // 0 = uninitialised
};

} // namespace x11
