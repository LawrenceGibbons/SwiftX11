//
//  CursorOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/10/26.
//

#pragma once

#include <cstdint>
#include "XProtoRegistrar.hpp"

namespace x11 {

class XProtoContext;
class ByteReader;

class CursorOps {
public:
  explicit CursorOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  // 93: CreateCursor
  void handleCreateCursor(XProtoContext& ctx, uint16_t seq, ByteReader& br);

  // 94: CreateGlyphCursor
  void handleCreateGlyphCursor(XProtoContext& ctx, uint16_t seq, ByteReader& br);

  // 95: FreeCursor
  void handleFreeCursor(XProtoContext& ctx, uint16_t seq, ByteReader& br);

  // 96: RecolorCursor
  void handleRecolorCursor(XProtoContext& ctx, uint16_t seq, ByteReader& br);
};

} // namespace x11
