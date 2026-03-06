//
//  AtomOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "Ops/AtomOps.hpp"
#include "Core/AtomTable.hpp"
#include "Core/XProtoContext.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/ByteReader.hpp"
#include "Core/X11CoreOpcodes.hpp"

namespace x11 {

AtomOps::AtomOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::InternAtom , &AtomOps::onMajor, this); // InternAtom
  reg.registerMajor(x11::opcode::GetAtomName, &AtomOps::onMajor, this); // GetAtomName
}

void AtomOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<AtomOps*>(user)->handle(ctx, dc);
}

void AtomOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case 16: handleInternAtom(ctx, dc.seq, /*onlyIfExists=*/(dc.minor != 0), dc.br); return;
    case 17: handleGetAtomName(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[AtomOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// ---- 16: InternAtom ----
// Request body after 4-byte header:
//   CARD16 name_len
//   CARD16 pad
//   name bytes (padded to 4)
  void AtomOps::handleInternAtom(XProtoContext& ctx, uint16_t seq, uint8_t onlyIfExists, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }

    const uint16_t nameLen = br.readU16();
    (void)br.readU16(); // pad

    const std::size_t avail = br.remaining();
    const std::size_t n = (nameLen < avail) ? nameLen : avail;

    const uint8_t* namePtr = br.ptr();
    br.skip(br.remaining());

    const bool only = (onlyIfExists != 0);
    uint32_t atom = 0;
    if (n > 0 && namePtr) {
      atom = AtomTable::instance().intern(reinterpret_cast<const char*>(namePtr), n, only);
    } else {
      atom = AtomTable::instance().intern("", 0, only);
    }

    (void)ctx.reply().sendInternAtomReply(seq, atom);
  }

  
// ---- 17: GetAtomName ----
// Request body after 4-byte header:
//   CARD32 atom
// Reply: 32-byte header + padded name bytes.
//   rep[8..9] nameLen
//   length_words = padded_name_bytes/4
  void AtomOps::handleGetAtomName(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }

    const uint32_t atom = br.readU32();
    br.skip(br.remaining());
    ctx.tracef("[AtomOps] GetAtomName seq=%u atom=0x%08X\n", (unsigned)seq, (unsigned)atom);

    uint32_t nameLen = 0;
    const char* name = AtomTable::instance().name(atom, &nameLen);

    if (!name) { name = ""; nameLen = 0; } // defensive; name() returns "" for unknown

    // BadAtom if atom is non-zero but not found
    if (atom != 0 && nameLen == 0) {
      ctx.transport().sendErrorCore(x11::error::BadAtom, seq, atom, x11::opcode::GetAtomName);
      return;
    }

    if (nameLen > 65535u) nameLen = 65535u;

    (void)ctx.reply().sendGetAtomNameReply(seq, name, static_cast<uint16_t>(nameLen));
  }


} // namespace x11
