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

// XKEYBOARD per-client state (xorg keeps the same split: `xkbClientFlags`
// and `mapNotifyMask`/`newKeyboardNotifyMask` on ClientRec, the per-device
// interest masks in XkbInterestRec).  Every XKB request except UseExtension
// is refused with BadAccess until the client has done UseExtension
// (xkb/xkb.c `_XkbClientInitialized`).
struct XkbClientState {
  bool     initialised = false;
  uint32_t pcfFlags = 0;                 // XkbPCF_* (DetectableAutoRepeat etc.)
  uint32_t autoCtrls = 0, autoCtrlValues = 0;
  // SelectEvents interest masks, per XkbSelectEventsReq detail sizes.
  uint16_t newKeyboardNotifyMask = 0;
  uint16_t mapNotifyMask = 0;
  uint16_t stateNotifyMask = 0;
  uint32_t ctrlsNotifyMask = 0;
  uint32_t iStateNotifyMask = 0;
  uint32_t iMapNotifyMask = 0;
  uint16_t namesNotifyMask = 0;
  uint8_t  compatNotifyMask = 0;
  uint8_t  bellNotifyMask = 0;
  uint8_t  actionMessageMask = 0;
  uint16_t accessXNotifyMask = 0;
  uint16_t extDevNotifyMask = 0;
};

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

  // SetCloseDownMode (opcode 112): 0 = DestroyAll (default, X11 spec),
  // 1 = RetainPermanent, 2 = RetainTemporary.  When non-zero, the client's
  // resources (windows, GCs, pixmaps, etc.) survive its disconnect.
  // Java AWT uses RetainPermanent for short-lived XDND helper connections
  // that create proxy windows the main JVM connection still needs after
  // the helper closes.  See XProtoDaemon::removeClient for the retain path.
  uint8_t closeDownMode() const { return close_down_mode_; }
  void setCloseDownMode(uint8_t v) { close_down_mode_ = v; }

  // XC-MISC: allocate a range of XIDs from this client's ID space.
  // Xlib allocates from the bottom (1, 2, 3…); we allocate from the top
  // downward to avoid collision until the client exhausts ~8M XIDs.
  // Returns (start_xid, count).  (0, 0) means exhausted.
  std::pair<uint32_t, uint32_t> allocXIDRange(uint32_t requested);

  // XC-MISC: allocate individual XIDs (up to count).  Returns actual count.
  uint32_t allocXIDList(uint32_t* out, uint32_t count);

  // XKEYBOARD state (Extensions/XKBOps.cpp).
  XkbClientState& xkb() { return xkb_; }
  const XkbClientState& xkb() const { return xkb_; }

  // XInput2 version negotiated by XIQueryVersion (0.0 = not yet asked), as
  // xorg's XIClientPrivate (Xi/xiqueryversion.c); XIQueryPointer consults
  // it for the 2.2 touch rule (Phase E, M13).
  uint16_t xi2Major() const { return xi2_major_; }
  uint16_t xi2Minor() const { return xi2_minor_; }
  void setXI2Version(uint16_t major, uint16_t minor) { xi2_major_ = major; xi2_minor_ = minor; }

private:
  XkbClientState xkb_{};
  uint16_t xi2_major_ = 0, xi2_minor_ = 0;
  int fd_;
  uint32_t rid_base_;
  uint32_t rid_mask_;
  XProtoTransport transport_;
  ReplyWriter reply_;
  bool big_req_enabled_ = false;
  uint8_t close_down_mode_ = 0;  // 0=DestroyAll, 1=RetainPermanent, 2=RetainTemporary
  // XC-MISC: cursor for XID allocation, starts at rid_mask/2 and grows upward
  uint32_t xid_alloc_cursor_ = 0;  // 0 = uninitialised
};

} // namespace x11
