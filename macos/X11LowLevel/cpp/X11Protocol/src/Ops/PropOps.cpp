//
//  PropOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//


#include "Ops/PropOps.hpp"
#include "Core/PropertyTable.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include "Core/XConstants.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/ByteReader.hpp"
#include "Utils/WireLE.hpp"
#include "Core/X11CoreOpcodes.hpp"

namespace x11 {

// -----------------------------
// PropOps wiring
// -----------------------------
PropOps::PropOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::ChangeProperty,  &PropOps::onMajor, this); // 18 ChangeProperty
  reg.registerMajor(x11::opcode::DeleteProperty,   &PropOps::onMajor, this); // 19 DeleteProperty
  reg.registerMajor(x11::opcode::GetProperty,      &PropOps::onMajor, this); // 20 GetProperty
  reg.registerMajor(x11::opcode::ListProperties,   &PropOps::onMajor, this); // 21 ListProperties
  reg.registerMajor(x11::opcode::RotateProperties, &PropOps::onMajor, this); // 114 RotateProperties
}

void PropOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<PropOps*>(user)->handle(ctx, dc);
}

void PropOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::ChangeProperty: handleChangeProperty(ctx, dc.seq, dc.minor /*mode*/, dc.br); return;
    case x11::opcode::DeleteProperty: handleDeleteProperty(ctx, dc.seq, dc.br); return;
    case x11::opcode::GetProperty   : handleGetProperty(ctx, dc.seq, dc.minor /*deleteFlag*/, dc.br); return;
    case x11::opcode::ListProperties:   handleListProperties(ctx, dc.seq, dc.br); return;
    case x11::opcode::RotateProperties: handleRotateProperties(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[PropOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// -----------------------------
// ChangeProperty (major 18)
// mode: 0 Replace, 1 Prepend, 2 Append
// body (20 bytes + data):
//   CARD32 window
//   CARD32 property
//   CARD32 type
//   CARD8  format (8/16/32)
//   3 pad
//   CARD32 nUnits
//   data...
// -----------------------------
void PropOps::handleChangeProperty(XProtoContext& ctx, uint16_t seq, uint8_t mode, ByteReader& br) {
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t wid   = br.readU32();

  // Validate window exists (allow root XID 0 and 1)
  if (wid != 0 && wid != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(wid, tmp)) {
      br.skip(br.remaining());
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::ChangeProperty);
      return;
    }
  }
  const uint32_t atom  = br.readU32();
  const uint32_t type  = br.readU32();
  const uint8_t  fmt   = br.readU8();
  br.skip(3); // pad
  const uint32_t nUnits = br.readU32();

  uint32_t unitBytes = 0;
  if (fmt == 8) unitBytes = 1;
  else if (fmt == 16) unitBytes = 2;
  else if (fmt == 32) unitBytes = 4;
  else {
    br.skip(br.remaining());
    return;
  }

  const uint64_t dataBytes64 = uint64_t(nUnits) * uint64_t(unitBytes);
  if (dataBytes64 > br.remaining()) {
    // malformed / truncated
    br.skip(br.remaining());
    return;
  }
  const std::size_t dataBytes = (std::size_t)dataBytes64;

  const uint8_t* data = br.ptr();
  br.skip(br.remaining()); // consume including pad

  // Apply
  if (mode == 1) {
    PropertyTable::instance().setAppend(wid, atom, type, fmt, data, dataBytes, /*append*/false);
  } else if (mode == 2) {
    PropertyTable::instance().setAppend(wid, atom, type, fmt, data, dataBytes, /*append*/true);
  } else {
    PropertyTable::instance().setReplace(wid, atom, type, fmt, data, dataBytes);
  }
}

// -----------------------------
// DeleteProperty (major 19)
// body: CARD32 window, CARD32 property
// -----------------------------
void PropOps::handleDeleteProperty(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }
  const uint32_t wid  = br.readU32();
  const uint32_t atom = br.readU32();
  br.skip(br.remaining());

  // Validate window exists (allow root XID 0 and 1)
  if (wid != 0 && wid != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(wid, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::DeleteProperty);
      return;
    }
  }

  PropertyTable::instance().erase(wid, atom);
  ctx.tracef("[PropOps] DeleteProperty wid=0x%08X atom=%u\n", (unsigned)wid, (unsigned)atom);
}

// -----------------------------
// GetProperty (major 20)
// body (20 bytes):
//   CARD32 window
//   CARD32 property
//   CARD32 type (0 = AnyPropertyType)
//   CARD32 longOffset (4-byte units)
//   CARD32 longLength (4-byte units)
// reply:
//   rep[1]=format (0 if none)
//   rep[8..11]=type
//   rep[12..15]=bytesAfter
//   rep[16..19]=nItems
//   payload padded to 4
// deleteFlag is dc.minor (0/1)
// -----------------------------
void PropOps::handleGetProperty(XProtoContext& ctx, uint16_t seq, uint8_t deleteFlag, ByteReader& br) {
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t wid      = br.readU32();
  const uint32_t atom     = br.readU32();
  const uint32_t reqType  = br.readU32();
  const uint32_t longOff  = br.readU32();
  const uint32_t longLen  = br.readU32();
  br.skip(br.remaining());

  // Validate window exists (allow root XID 0 and 1)
  if (wid != 0 && wid != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(wid, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::GetProperty);
      return;
    }
  }

  PropertyTable::Prop p{};
  const bool found = PropertyTable::instance().get(wid, atom, p);

  // “no such property”
  if (!found || p.format == 0 || p.data.empty()) {
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t,32>& rep) {
      rep[1] = 0;                       // format
      wire::wr32_le(rep.data()+8,  0); // type=None
      wire::wr32_le(rep.data()+12, 0); // bytesAfter
      wire::wr32_le(rep.data()+16, 0); // nItems
    });
    return;
  }

  // Type mismatch => empty (but property exists)
  if (reqType != 0 && p.type != reqType) {
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t,32>& rep) {
      rep[1] = 0;                          // format = 0
      wire::wr32_le(rep.data()+8,  p.type); // type = actual property type
      wire::wr32_le(rep.data()+12, 0);      // bytesAfter = 0
      wire::wr32_le(rep.data()+16, 0);      // nItems = 0
    });
    return;
  }

  uint32_t unitBytes = 0;
  if (p.format == 8) unitBytes = 1;
  else if (p.format == 16) unitBytes = 2;
  else if (p.format == 32) unitBytes = 4;
  else {
    // invalid stored property
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t,32>& rep) {
      rep[1] = 0;
      wire::wr32_le(rep.data()+8,  0);
      wire::wr32_le(rep.data()+12, 0);
      wire::wr32_le(rep.data()+16, 0);
    });
    return;
  }

  const uint32_t totalBytes = (uint32_t)std::min<std::size_t>(p.data.size(), 0xFFFFFFFFu);

  const uint64_t byteOff64  = uint64_t(longOff) * 4ull;
  const uint64_t maxBytes64 = uint64_t(longLen) * 4ull;

  uint32_t sendOff = (byteOff64 >= totalBytes) ? totalBytes : (uint32_t)byteOff64;
  uint32_t remainBytes = totalBytes - sendOff;

  uint32_t sendBytes = remainBytes;
  if (maxBytes64 < sendBytes) sendBytes = (uint32_t)maxBytes64;

  // For 16/32 formats: only whole items
  if (unitBytes > 1) {
    sendBytes -= (sendBytes % unitBytes);
  }

  const uint32_t bytesAfter = remainBytes - sendBytes;
  const uint32_t nItems = (unitBytes ? (sendBytes / unitBytes) : 0);

  const uint32_t paddedBytes = (sendBytes + 3u) & ~3u;
  const uint32_t lengthWords = paddedBytes / 4u;

  std::array<uint8_t,32> rep{};
  rep.fill(0);
  rep[0] = 1;
  rep[2] = (uint8_t)(seq & 0xFF);
  rep[3] = (uint8_t)((seq >> 8) & 0xFF);

  rep[1] = p.format;
  wire::wr32_le(rep.data()+4,  lengthWords);
  wire::wr32_le(rep.data()+8,  p.type);
  wire::wr32_le(rep.data()+12, bytesAfter);
  wire::wr32_le(rep.data()+16, nItems);

  // Build contiguous payload (unpadded), let ReplyWriter pad once if needed
  std::vector<uint8_t> payload;
  payload.resize(sendBytes);
  if (sendBytes) {
    memcpy(payload.data(), p.data.data() + sendOff, sendBytes);
  }

  const void* payloadPtr = payload.empty() ? nullptr : payload.data();
  const bool ok = ctx.reply().sendReplyWithPaddedPayload(rep.data(),
                                                        payloadPtr,
                                                        payload.size());
  if (!ok) return;
  
  // Delete property if requested and we returned the entire property starting at offset 0
  if (deleteFlag && longOff == 0 && sendOff == 0 && sendBytes == totalBytes) {
    PropertyTable::instance().erase(wid, atom);
  }
}
  
  
void PropOps::handleListProperties(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  // Request body: WINDOW
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  if (br.remaining()) br.skip(br.remaining());

  // Validate window exists (allow root XID 0 and 1)
  if (wid != 0 && wid != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(wid, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::ListProperties);
      return;
    }
  }

  std::vector<uint32_t> atoms;
  PropertyTable::instance().listAtoms(wid, atoms);

  const uint16_t n = (uint16_t)std::min<size_t>(atoms.size(), 0xFFFFu);
  const uint32_t payloadBytes = uint32_t(n) * 4u;     // CARD32 atoms
  const uint32_t lengthWords  = payloadBytes / 4u;    // == n

  std::array<uint8_t, 32> rep{};
  rep.fill(0);
  rep[0] = 1; // Reply
  // rep[1] unused/pad
  wire::wr16_le(rep.data() + 2, seq);
  wire::wr32_le(rep.data() + 4, lengthWords);
  wire::wr16_le(rep.data() + 8, n);  // nProperties

  // Build payload as packed LE CARD32s
  std::vector<uint8_t> payload(payloadBytes);
  for (uint16_t i = 0; i < n; i++) {
    wire::wr32_le(payload.data() + 4u * i, atoms[i]);
  }

  // Use ReplyWriter so your debug tracker stays coherent.
  // payloadBytes is already multiple-of-4 so padding is a no-op.
  (void)ctx.reply().sendReplyWithPaddedPayload(rep.data(),
                                              payload.empty() ? nullptr : payload.data(),
                                              payload.size());
}
  
  

// major 114 RotateProperties (void stub)
// Request: CARD32 window + CARD16 nProps + INT16 delta + nProps*CARD32 atoms
void PropOps::handleRotateProperties(XProtoContext& /*ctx*/, uint16_t /*seq*/, ByteReader& br) {
  br.skip(br.remaining());
}

} // namespace x11
