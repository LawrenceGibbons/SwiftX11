//
//  GCOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once

#include <cstdint>
#include <cstddef>

#include <Core/XProtoRegistrar.hpp>

namespace x11 {

// Forward decls to avoid including everything in headers.
class XProtoContext;
class ByteReader;
struct GCState;
  

// Core “GC ops” (Graphics Context):
//   major 55 CreateGC
//   major 56 ChangeGC
//   major 57 CopyGC (optional later)
//   major 58 SetDashes (later)
//   major 59 SetClipRectangles (later)
//   major 60 FreeGC
//
class GCOps {
public:
  explicit GCOps(XProtoRegistrar& reg);


private:
  static void onMajor(void* user, XProtoContext& ctx, DispatchContext& dc);

  // Entry point from dispatcher for opcodes in the GC family.
  // `minor` is unused for these core requests but kept for uniformity.
  void handle(XProtoContext& ctx, DispatchContext& dc);

  void handleCreateGC(XProtoContext& ctx, uint16_t seq, ByteReader& br);  // major 55
  void handleChangeGC(XProtoContext& ctx, uint16_t seq, ByteReader& br);  // major 56
  void handleFreeGC  (XProtoContext& ctx, uint16_t seq, ByteReader& br);  // major 60

  static uint32_t mapPixelToARGB(uint32_t val);
  static void applyValueMask(uint32_t vmask, ByteReader& br, GCState& st);
};

} // namespace x11
