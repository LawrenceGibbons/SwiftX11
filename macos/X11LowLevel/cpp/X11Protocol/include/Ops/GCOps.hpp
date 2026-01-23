//
//  GCOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once

#include <cstdint>
#include <cstddef>

namespace x11 {

// Forward decls to avoid including everything in headers.
class XProtoContext;
class ByteReader;

// Core “GC ops” (Graphics Context):
//   major 55 CreateGC
//   major 56 ChangeGC
//   major 57 CopyGC (optional later)
//   major 58 SetDashes (later)
//   major 59 SetClipRectangles (later)
//   major 60 FreeGC
//
// For now: stubs only. We’ll implement minimal fg/bg, lineWidth, fillStyle, etc. later.
class GCOps {
public:
  explicit GCOps(XProtoContext& ctx) : ctx_(ctx) {}

  // Entry point from dispatcher for opcodes in the GC family.
  // `minor` is unused for these core requests but kept for uniformity.
  void handle(uint8_t majorOpcode, uint8_t minorOpcode, ByteReader& br);

private:
  void handleCreateGC(uint8_t minorOpcode, ByteReader& br);   // major 55
  void handleChangeGC(uint8_t minorOpcode, ByteReader& br);   // major 56
  void handleFreeGC(uint8_t minorOpcode, ByteReader& br);     // major 60

  // Helpers (stubs for now)
  void skipValueList(uint32_t valueMask, ByteReader& br);

private:
  XProtoContext& ctx_;
};

} // namespace x11
