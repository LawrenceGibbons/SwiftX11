//
//  SelectionOps.cpp
//  X11LowLevel
//

#include "Ops/SelectionOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Utils/ByteReader.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/WireLE.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include <mutex>
#include <unordered_map>

namespace x11 {

// ---------------------------------------------------------------------------
// Static selection table: atom → owner window XID
// ---------------------------------------------------------------------------
static std::mutex sSelMtx;
static std::unordered_map<uint32_t /*atom*/, uint32_t /*owner_wid*/> sSelOwner;

// ---------------------------------------------------------------------------
// Construction — register opcodes 22–25
// ---------------------------------------------------------------------------
SelectionOps::SelectionOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::SetSelectionOwner, &SelectionOps::onMajor, this); // 22
  reg.registerMajor(x11::opcode::GetSelectionOwner, &SelectionOps::onMajor, this); // 23
  reg.registerMajor(x11::opcode::ConvertSelection,  &SelectionOps::onMajor, this); // 24
  reg.registerMajor(x11::opcode::SendEvent,         &SelectionOps::onMajor, this); // 25
}

void SelectionOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<SelectionOps*>(user)->handle(ctx, dc);
}

void SelectionOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::SetSelectionOwner: handleSetSelectionOwner(ctx, dc.seq, dc.br); return;
    case x11::opcode::GetSelectionOwner: handleGetSelectionOwner(ctx, dc.seq, dc.br); return;
    case x11::opcode::ConvertSelection:  handleConvertSelection(ctx, dc.seq, dc.br); return;
    case x11::opcode::SendEvent:         handleSendEvent(ctx, dc.seq, dc.minor, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      return;
  }
}

// ---------------------------------------------------------------------------
// 22 SetSelectionOwner  (void — no reply)
// Body (12 bytes): CARD32 owner, CARD32 selection, CARD32 time
// ---------------------------------------------------------------------------
void SelectionOps::handleSetSelectionOwner(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }

  const uint32_t owner     = br.readU32();
  const uint32_t selection = br.readU32();
  const uint32_t time      = br.readU32();

  br.skip(br.remaining());

  uint32_t prevOwner = 0;
  {
    std::lock_guard<std::mutex> lk(sSelMtx);
    auto it = sSelOwner.find(selection);
    if (it != sSelOwner.end()) {
      prevOwner = it->second;
      it->second = owner;
    } else {
      sSelOwner[selection] = owner;
    }
  }

  // If previous owner was different and non-zero, send SelectionClear (type 29)
  if (prevOwner != 0 && prevOwner != owner) {
    uint8_t ev[32] = {0};
    ev[0] = 29; // SelectionClear
    wire::wr32_le(ev + 4,  time);
    wire::wr32_le(ev + 8,  prevOwner);  // window
    wire::wr32_le(ev + 12, selection);  // atom
    (void)ctx.transport().sendEvent32(prevOwner, ev);
  }

#ifdef X11_TRACE_VERBOSE
  ctx.tracef("[SelectionOps] SetSelectionOwner sel=0x%08X owner=0x%08X prev=0x%08X\n",
             (unsigned)selection, (unsigned)owner, (unsigned)prevOwner);
#endif
}

// ---------------------------------------------------------------------------
// 23 GetSelectionOwner  (reply)
// Body (4 bytes): CARD32 selection
// Reply: bytes 8..11 = owner window
// ---------------------------------------------------------------------------
void SelectionOps::handleGetSelectionOwner(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }

  const uint32_t selection = br.readU32();

  br.skip(br.remaining());

  uint32_t owner = 0;
  {
    std::lock_guard<std::mutex> lk(sSelMtx);
    auto it = sSelOwner.find(selection);
    if (it != sSelOwner.end()) owner = it->second;
  }

  ReplyWriter rw(ctx.transport());
  rw.sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    wire::wr32_le(rep.data() + 8, owner);
  });

#ifdef X11_TRACE_VERBOSE
  ctx.tracef("[SelectionOps] GetSelectionOwner sel=0x%08X → owner=0x%08X\n",
             (unsigned)selection, (unsigned)owner);
#endif
}

// ---------------------------------------------------------------------------
// 24 ConvertSelection  (void)
// Body (20 bytes): CARD32 requestor, CARD32 selection, CARD32 target,
//                  CARD32 property, CARD32 time
// ---------------------------------------------------------------------------
void SelectionOps::handleConvertSelection(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t requestor = br.readU32();
  const uint32_t selection = br.readU32();
  const uint32_t target    = br.readU32();
  const uint32_t property  = br.readU32();
  const uint32_t time      = br.readU32();

  br.skip(br.remaining());

  uint32_t owner = 0;
  {
    std::lock_guard<std::mutex> lk(sSelMtx);
    auto it = sSelOwner.find(selection);
    if (it != sSelOwner.end()) owner = it->second;
  }

  if (owner != 0) {
    // Send SelectionRequest (type 30) to owner
    uint8_t ev[32] = {0};
    ev[0] = 30; // SelectionRequest
    wire::wr32_le(ev + 4,  time);
    wire::wr32_le(ev + 8,  owner);
    wire::wr32_le(ev + 12, requestor);
    wire::wr32_le(ev + 16, selection);
    wire::wr32_le(ev + 20, target);
    wire::wr32_le(ev + 24, property);
    (void)ctx.transport().sendEvent32(owner, ev);
  } else {
    // No owner — send SelectionNotify (type 31) to requestor with property=None
    uint8_t ev[32] = {0};
    ev[0] = 31; // SelectionNotify
    wire::wr32_le(ev + 4,  time);
    wire::wr32_le(ev + 8,  requestor);
    wire::wr32_le(ev + 12, selection);
    wire::wr32_le(ev + 16, target);
    wire::wr32_le(ev + 20, 0); // property = None
    (void)ctx.transport().sendEvent32(requestor, ev);
  }

#ifdef X11_TRACE_VERBOSE
  ctx.tracef("[SelectionOps] ConvertSelection sel=0x%08X req=0x%08X owner=0x%08X\n",
             (unsigned)selection, (unsigned)requestor, (unsigned)owner);
#endif
}

// ---------------------------------------------------------------------------
// 25 SendEvent  (void)
// dc.minor = propagate byte
// Body (36 bytes): CARD32 destination, CARD32 eventMask, BYTE[32] event
// ---------------------------------------------------------------------------
void SelectionOps::handleSendEvent(XProtoContext& ctx, uint16_t /*seq*/, uint8_t propagate, ByteReader& br) {
  (void)propagate;

  if (br.remaining() < 36) { br.skip(br.remaining()); return; }

  const uint32_t destination = br.readU32();
  const uint32_t eventMask   = br.readU32();
  (void)eventMask;

  uint8_t event[32];
  for (int i = 0; i < 32; ++i) event[i] = br.readU8();

  br.skip(br.remaining());

  // Set bit 0x80 on event[0] (SendEvent flag per X11 spec)
  event[0] |= 0x80u;

  // Resolve special destinations
  uint32_t resolvedDest = destination;
  if (destination == 0) {
    // PointerWindow
    resolvedDest = ctx.input().last_xid;
  } else if (destination == 1) {
    // InputFocus
    resolvedDest = ctx.input().focus_xid;
  }

  if (resolvedDest != 0) {
    (void)ctx.transport().sendEvent32(resolvedDest, event);
  }

#ifdef X11_TRACE_VERBOSE
  ctx.tracef("[SelectionOps] SendEvent dest=0x%08X resolved=0x%08X type=%u\n",
             (unsigned)destination, (unsigned)resolvedDest, (unsigned)(event[0] & 0x7Fu));
#endif
}

} // namespace x11
