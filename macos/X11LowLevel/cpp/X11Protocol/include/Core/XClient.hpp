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

private:
  int fd_;
  uint32_t rid_base_;
  uint32_t rid_mask_;
  XProtoTransport transport_;
  ReplyWriter reply_;
  bool big_req_enabled_ = false;
};

} // namespace x11
