//
//  AtomOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once

#include <cstdint>
#include <Core/XProtoRegistrar.hpp>

namespace x11 {

class XProtoContext;
class ByteReader;

class AtomOps {
public:
  explicit AtomOps(XProtoRegistrar& reg);

private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);
  void handle(XProtoContext& ctx, DispatchContext& dc);

  // Major 16: InternAtom
  void handleInternAtom(XProtoContext& ctx, uint16_t seq, uint8_t onlyIfExists, ByteReader& br);

  // Major 17: GetAtomName
  void handleGetAtomName(XProtoContext& ctx, uint16_t seq, ByteReader& br);
};

} // namespace x11

