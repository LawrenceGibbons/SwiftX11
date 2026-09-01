//
//  SelectionOps.hpp
//  X11LowLevel
//

#pragma once
#include <cstdint>
#include "Core/XProtoRegistrar.hpp"

namespace x11 {

class XProtoContext;
class ByteReader;

class SelectionOps {
public:
  explicit SelectionOps(XProtoRegistrar& reg);

  /// Called on FocusIn: if macOS clipboard changed since last check,
  /// claim PRIMARY+CLIPBOARD so the next paste serves macOS content.
  static void claimSelectionsIfMacOSChanged(XProtoContext& ctx);

  /// INCR receive (server-as-requestor): large X11→macOS clipboard
  /// captures arrive as chunked INCR transfers.  PropOps calls these from
  /// ChangeProperty so each chunk written to the proxy requestor (root)
  /// is consumed, acknowledged with PropertyNotify(Deleted), and
  /// accumulated; a zero-length chunk completes the transfer and pushes
  /// the text to NSPasteboard.  xproto thread only.
  static bool incrReceiveActive(uint32_t wid, uint32_t prop);
  static void incrOnChunk(XProtoContext& ctx, uint32_t wid, uint32_t prop,
                          uint32_t type, uint8_t format,
                          const uint8_t* data, uint64_t len);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  void handleSetSelectionOwner(XProtoContext& ctx, uint16_t seq, ByteReader& br);  // 22
  void handleGetSelectionOwner(XProtoContext& ctx, uint16_t seq, ByteReader& br);  // 23 (reply)
  void handleConvertSelection(XProtoContext& ctx, uint16_t seq, ByteReader& br);   // 24
  void handleSendEvent(XProtoContext& ctx, uint16_t seq, uint8_t propagate, ByteReader& br); // 25
};

} // namespace x11
