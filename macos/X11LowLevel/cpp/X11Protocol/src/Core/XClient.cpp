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

} // namespace x11
