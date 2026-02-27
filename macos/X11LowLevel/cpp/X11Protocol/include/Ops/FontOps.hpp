//
//  FontOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#pragma once
#include <cstdint>
#include <Core/XProtoRegistrar.hpp>

namespace x11 {

class XProtoContext;
class ByteReader;

class FontOps {
public:
  explicit FontOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  // 45 OpenFont
  void handleOpenFont(XProtoContext& ctx, uint16_t seq, ByteReader& br);

  // 46 CloseFont
  void handleCloseFont(XProtoContext& ctx, uint16_t seq, ByteReader& br);

  // 47 QueryFont (reply)
  void handleQueryFont(XProtoContext& ctx, uint16_t seq, ByteReader& br);

  // 48 QueryTextExtents (reply)
  void handleQueryTextExtents(XProtoContext& ctx, uint16_t seq, bool oddLength, ByteReader& br);

  // 49 ListFonts (reply)
  void handleListFonts(XProtoContext& ctx, uint16_t seq, ByteReader& br);

  // 50 ListFontsWithInfo (reply)
  void handleListFontsWithInfo(XProtoContext& ctx, uint16_t seq, ByteReader& br);

  // 97 QueryBestSize
  void handleQueryBestSize(XProtoContext& ctx, uint16_t seq, uint8_t class_, ByteReader& br);

private:
  // Bring-up: track “opened” font IDs so QueryFont can succeed.
  // (Later replace with a proper FontTable and font metrics.)
  // Kept in .cpp as static storage.
};

} // namespace x11
