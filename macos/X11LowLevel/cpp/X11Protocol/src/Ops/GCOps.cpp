//
//  GCOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "GCOps.hpp"

// You’ll replace these with your real infrastructure types as your skeleton firms up.
#include <cstdio>

namespace x11 {

// Minimal placeholder “reader” expectations:
// - br.readU32(), br.readI16(), br.readU16(), br.skip(n)
// - br.remaining()
//
// If your ByteReader API differs, adjust the calls in one place here.
class ByteReader {
public:
  uint32_t readU32();
  uint16_t readU16();
  int16_t  readI16();
  void     skip(size_t n);
  std::size_t   remaining() const;
};

// Minimal placeholder “context” expectations:
// - ctx.tracef(...)
// - ctx.gcStore() / ctx.state() etc (later)
class XProtoContext {
public:
  void tracef(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};

void GCOps::handle(uint8_t majorOpcode, uint8_t minorOpcode, ByteReader& br) {
  switch (majorOpcode) {
    case 55: handleCreateGC(minorOpcode, br); break;
    case 56: handleChangeGC(minorOpcode, br); break;
    case 60: handleFreeGC(minorOpcode, br); break;
    default:
      // Not ours; caller shouldn't route here for other opcodes.
      ctx_.tracef("[GCOps] unexpected major=%u (minor=%u)\n",
                  (unsigned)majorOpcode, (unsigned)minorOpcode);
      // Best effort: consume nothing.
      break;
  }
}

void GCOps::handleCreateGC(uint8_t /*minorOpcode*/, ByteReader& br) {
  // X11 CreateGC request body (after 4-byte header):
  //   CARD32 gc
  //   CARD32 drawable
  //   CARD32 valueMask
  //   LISTofCARD32 values
  if (br.remaining() < 12) {
    ctx_.tracef("[GCOps] CreateGC: short body (%zu)\n", br.remaining());
    // In a strict server we’d send BadLength, but for bring-up we just bail.
    return;
  }

  const uint32_t gcXid     = br.readU32();
  const uint32_t drawable  = br.readU32();
  const uint32_t valueMask = br.readU32();

  ctx_.tracef("[GCOps] CreateGC gc=0x%08X drawable=0x%08X vmask=0x%08X (stub)\n",
              gcXid, drawable, valueMask);

  // TODO: allocate/overwrite GC object in GCStore.
  // TODO: parse value list (foreground/background, lineWidth, fillStyle...).
  skipValueList(valueMask, br);
}

void GCOps::handleChangeGC(uint8_t /*minorOpcode*/, ByteReader& br) {
  // X11 ChangeGC request body:
  //   CARD32 gc
  //   CARD32 valueMask
  //   LISTofCARD32 values
  if (br.remaining() < 8) {
    ctx_.tracef("[GCOps] ChangeGC: short body (%zu)\n", br.remaining());
    return;
  }

  const uint32_t gcXid     = br.readU32();
  const uint32_t valueMask = br.readU32();

  ctx_.tracef("[GCOps] ChangeGC gc=0x%08X vmask=0x%08X (stub)\n", gcXid, valueMask);

  // TODO: lookup GC object; apply value list updates.
  skipValueList(valueMask, br);
}

void GCOps::handleFreeGC(uint8_t /*minorOpcode*/, ByteReader& br) {
  // X11 FreeGC request body:
  //   CARD32 gc
  if (br.remaining() < 4) {
    ctx_.tracef("[GCOps] FreeGC: short body (%zu)\n", br.remaining());
    return;
  }

  const uint32_t gcXid = br.readU32();
  ctx_.tracef("[GCOps] FreeGC gc=0x%08X (stub)\n", gcXid);

  // TODO: delete GC object from GCStore.
}

void GCOps::skipValueList(uint32_t valueMask, ByteReader& br) {
  // Each set bit in valueMask corresponds to one CARD32 in the value list,
  // in increasing bit order.
  // For now we don’t interpret it — we just consume it.
  //
  // Count bits set in 32-bit mask.
  uint32_t n = 0;
  uint32_t m = valueMask;
  while (m) {
    m &= (m - 1);
    n++;
  }

  const std::size_t bytes = (size_t)n * 4u;
  if (br.remaining() < bytes) {
    ctx_.tracef("[GCOps] skipValueList: wanted %zu bytes but only %zu remain\n",
                bytes, br.remaining());
    // Consume whatever remains to keep stream moving.
    br.skip(br.remaining());
    return;
  }

  br.skip(bytes);
}

} // namespace x11
