#include "Ops/GCOps.hpp"

#include "Core/XProtoContext.hpp"
#include "Utils/ByteReader.hpp"
#include "Core/GCTable.hpp"
#include "Core/X11CoreOpcodes.hpp"

namespace x11 {

GCOps::GCOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::CreateGC, &GCOps::onMajor, this); // CreateGC
  reg.registerMajor(x11::opcode::ChangeGC, &GCOps::onMajor, this); // ChangeGC
  reg.registerMajor(x11::opcode::FreeGC,   &GCOps::onMajor, this); // FreeGC
}

void GCOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<GCOps*>(user)->handle(ctx, dc);
}

void GCOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::CreateGC: handleCreateGC(ctx, dc.seq, dc.br); return;
    case x11::opcode::ChangeGC: handleChangeGC(ctx, dc.seq, dc.br); return;
    case x11::opcode::FreeGC  : handleFreeGC(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[GCOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// Keep your bring-up mapping rules.
uint32_t GCOps::mapPixelToARGB(uint32_t val) {
  if (val == 0) return 0xFF000000u;
  if (val == 1) return 0xFFFFFFFFu;
  return 0xFF000000u | (val & 0x00FFFFFFu);
}

// Parse value list in increasing bit order.
// We only care about bit 2 (GCForeground) and bit 3 (GCBackground),
// but we must still CONSUME all provided values to keep the stream aligned.
void GCOps::applyValueMask(uint32_t vmask, ByteReader& br, uint32_t& io_fg, uint32_t& io_bg) {
  for (uint32_t bit = 0; bit < 32; bit++) {
    if ((vmask & (1u << bit)) == 0) continue;
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t val = br.readU32();
    if (bit == 2) io_fg = mapPixelToARGB(val);
    else if (bit == 3) io_bg = mapPixelToARGB(val);
  }
}

// major 55 CreateGC
void GCOps::handleCreateGC(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  // Body:
  //   CARD32 gc
  //   CARD32 drawable
  //   CARD32 valueMask
  //   LISTofCARD32 values
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }

  const uint32_t gcXid    = br.readU32();
  (void)br.readU32(); // drawable (unused)
  const uint32_t vmask    = br.readU32();

  auto st = GCTable::instance().getOrCreate(gcXid);
  applyValueMask(vmask, br, st.fg, st.bg);
  // Consume any trailing padding (CreateGC has no fixed padding, but callers may pass more bytes)
  br.skip(br.remaining());

  GCTable::instance().upsert(st);

  // Optional debug:
  // ctx.tracef("[GCOps] CreateGC gc=0x%08X fg=0x%08X bg=0x%08X vmask=0x%08X\n",
  //            (unsigned)gcXid, (unsigned)st.fg, (unsigned)st.bg, (unsigned)vmask);
}

// major 56 ChangeGC
void GCOps::handleChangeGC(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  // Body:
  //   CARD32 gc
  //   CARD32 valueMask
  //   LISTofCARD32 values
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }

  const uint32_t gcXid = br.readU32();
  const uint32_t vmask = br.readU32();

  auto st = GCTable::instance().getOrCreate(gcXid);
  applyValueMask(vmask, br, st.fg, st.bg);
  br.skip(br.remaining());

  GCTable::instance().upsert(st);

  // ctx.tracef("[GCOps] ChangeGC gc=0x%08X fg=0x%08X bg=0x%08X vmask=0x%08X\n",
  //            (unsigned)gcXid, (unsigned)st.fg, (unsigned)st.bg, (unsigned)vmask);
}

// major 60 FreeGC
void GCOps::handleFreeGC(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t gcXid = br.readU32();
  br.skip(br.remaining());

  GCTable::instance().erase(gcXid);
  // ctx.tracef("[GCOps] FreeGC gc=0x%08X\n", (unsigned)gcXid);
}

} // namespace x11
