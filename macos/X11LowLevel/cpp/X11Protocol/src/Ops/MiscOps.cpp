//
//  MiscOps.cpp
//  X11LowLevel
//

#include "Ops/MiscOps.hpp"

#include <array>
#include <cstring>
#include <cstdio>

extern "C" {
#include "SwiftX11Bridge.h"
}

#include "Core/XProtoContext.hpp"
#include "Utils/ByteReader.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/WireLE.hpp"
#include "Core/X11CoreOpcodes.hpp"

namespace x11 {

MiscOps::MiscOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::ChangeKeyboardMapping , &MiscOps::onMajor, this); // 100
  reg.registerMajor(x11::opcode::ChangeKeyboardControl , &MiscOps::onMajor, this); // 102
  reg.registerMajor(x11::opcode::GetKeyboardControl    , &MiscOps::onMajor, this); // 103
  reg.registerMajor(x11::opcode::Bell                  , &MiscOps::onMajor, this); // 104
  reg.registerMajor(x11::opcode::ChangePointerControl  , &MiscOps::onMajor, this); // 105
  reg.registerMajor(x11::opcode::GetPointerControl     , &MiscOps::onMajor, this); // 106
  reg.registerMajor(x11::opcode::SetScreenSaver        , &MiscOps::onMajor, this); // 107
  reg.registerMajor(x11::opcode::GetScreenSaver        , &MiscOps::onMajor, this); // 108
  reg.registerMajor(x11::opcode::ChangeHosts           , &MiscOps::onMajor, this); // 109
  reg.registerMajor(x11::opcode::ListHosts             , &MiscOps::onMajor, this); // 110
  reg.registerMajor(x11::opcode::SetAccessControl      , &MiscOps::onMajor, this); // 111
  reg.registerMajor(x11::opcode::SetCloseDownMode      , &MiscOps::onMajor, this); // 112
  reg.registerMajor(x11::opcode::KillClient            , &MiscOps::onMajor, this); // 113
  reg.registerMajor(x11::opcode::ForceScreenSaver      , &MiscOps::onMajor, this); // 115
  reg.registerMajor(x11::opcode::NoOperation           , &MiscOps::onMajor, this); // 127
}

void MiscOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<MiscOps*>(user)->handle(ctx, dc);
}

void MiscOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::ChangeKeyboardMapping : handleChangeKeyboardMapping(ctx, dc.seq, dc.br); return;
    case x11::opcode::ChangeKeyboardControl : handleChangeKeyboardControl(ctx, dc.seq, dc.br); return;
    case x11::opcode::GetKeyboardControl    : handleGetKeyboardControl(ctx, dc.seq, dc.br);    return;
    case x11::opcode::Bell                  : handleBell(ctx, dc.seq, dc.br);                  return;
    case x11::opcode::ChangePointerControl  : handleChangePointerControl(ctx, dc.seq, dc.br);  return;
    case x11::opcode::GetPointerControl     : handleGetPointerControl(ctx, dc.seq, dc.br);     return;
    case x11::opcode::SetScreenSaver        : handleSetScreenSaver(ctx, dc.seq, dc.br);        return;
    case x11::opcode::GetScreenSaver        : handleGetScreenSaver(ctx, dc.seq, dc.br);        return;
    case x11::opcode::ChangeHosts           : handleChangeHosts(ctx, dc.seq, dc.br);           return;
    case x11::opcode::ListHosts             : handleListHosts(ctx, dc.seq, dc.br);             return;
    case x11::opcode::SetAccessControl      : handleSetAccessControl(ctx, dc.seq, dc.br);      return;
    case x11::opcode::SetCloseDownMode      : handleSetCloseDownMode(ctx, dc.seq, dc.br);      return;
    case x11::opcode::KillClient            : handleKillClient(ctx, dc.seq, dc.br);            return;
    case x11::opcode::ForceScreenSaver      : handleForceScreenSaver(ctx, dc.seq, dc.br);      return;
    case x11::opcode::NoOperation           : handleNoOperation(ctx, dc.seq, dc.br);           return;
    default:
      dc.br.skip(dc.br.remaining());
#ifdef X11_TRACE_VERBOSE
      ctx.tracef("[MiscOps] unexpected major=%u\n", (unsigned)dc.major);
#endif
      return;
  }
}

// =============================================================================
// ChangeKeyboardMapping (100) — void stub
// =============================================================================
void MiscOps::handleChangeKeyboardMapping(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  x11_ui_push_log(1, "[MiscOps] ChangeKeyboardMapping (100) stub\n");
#endif
  br.skip(br.remaining());
}

// =============================================================================
// ChangeKeyboardControl (102) — void stub
// =============================================================================
void MiscOps::handleChangeKeyboardControl(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  x11_ui_push_log(1, "[MiscOps] ChangeKeyboardControl (102) stub\n");
#endif
  br.skip(br.remaining());
}

// =============================================================================
// GetKeyboardControl (103) — reply
// =============================================================================
void MiscOps::handleGetKeyboardControl(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  ctx.tracef("[MiscOps] GetKeyboardControl (103) reply\n");
#endif
  br.skip(br.remaining());

  // Reply: 52 bytes total (32 header + 20 extra)
  // rep[1] = 1 (global auto repeat mode = On)
  // length = 5 (20 extra bytes / 4)
  // bytes 8-11: CARD32 led_mask
  // byte 12: CARD8 key_click_percent
  // byte 13: CARD8 bell_percent
  // bytes 14-15: CARD16 bell_pitch
  // bytes 16-17: CARD16 bell_duration
  // bytes 18-19: pad
  // bytes 20-51: 32 bytes of auto_repeats (all 0xFF for "on")

  const bool ok = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = 1; // globalAutoRepeat = On
    wire::wr32_le(rep.data() + 4, 5); // length = 5 (20 extra bytes)
    wire::wr32_le(rep.data() + 8, 0); // led_mask
    rep[12] = 0;   // key_click_percent
    rep[13] = 50;  // bell_percent
    wire::wr16_le(rep.data() + 14, 400); // bell_pitch
    wire::wr16_le(rep.data() + 16, 100); // bell_duration
    // rep[18..19] = pad (already 0)
    // rep[20..31] = first 12 bytes of auto_repeats (set to 0xFF)
    std::memset(rep.data() + 20, 0xFF, 12);
  });
  if (!ok) return;

  // Send remaining 20 bytes of auto_repeats (bytes 32-51)
  uint8_t extra[20];
  std::memset(extra, 0xFF, 20);
  (void)ctx.reply().sendReplyRaw(extra, 20);
}

// =============================================================================
// Bell (104) — void stub
// =============================================================================
void MiscOps::handleBell(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  x11_ui_push_log(1, "[MiscOps] Bell (104) stub\n");
#endif
  br.skip(br.remaining());
}

// =============================================================================
// ChangePointerControl (105) — void stub
// =============================================================================
void MiscOps::handleChangePointerControl(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  x11_ui_push_log(1, "[MiscOps] ChangePointerControl (105) stub\n");
#endif
  br.skip(br.remaining());
}

// =============================================================================
// GetPointerControl (106) — reply
// =============================================================================
void MiscOps::handleGetPointerControl(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  ctx.tracef("[MiscOps] GetPointerControl (106) reply\n");
#endif
  br.skip(br.remaining());

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    wire::wr16_le(rep.data() + 8, 2);   // acceleration-numerator
    wire::wr16_le(rep.data() + 10, 1);  // acceleration-denominator
    wire::wr16_le(rep.data() + 12, 4);  // threshold
  });
}

// =============================================================================
// SetScreenSaver (107) — void stub
// =============================================================================
void MiscOps::handleSetScreenSaver(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  x11_ui_push_log(1, "[MiscOps] SetScreenSaver (107) stub\n");
#endif
  br.skip(br.remaining());
}

// =============================================================================
// GetScreenSaver (108) — reply
// =============================================================================
void MiscOps::handleGetScreenSaver(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  ctx.tracef("[MiscOps] GetScreenSaver (108) reply\n");
#endif
  br.skip(br.remaining());

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    wire::wr16_le(rep.data() + 8, 0);   // timeout
    wire::wr16_le(rep.data() + 10, 0);  // interval
    rep[12] = 0; // prefer-blanking = No
    rep[13] = 0; // allow-exposures = No
  });
}

// =============================================================================
// ChangeHosts (109) — void stub
// =============================================================================
void MiscOps::handleChangeHosts(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  x11_ui_push_log(1, "[MiscOps] ChangeHosts (109) stub\n");
#endif
  br.skip(br.remaining());
}

// =============================================================================
// ListHosts (110) — reply
// =============================================================================
void MiscOps::handleListHosts(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  ctx.tracef("[MiscOps] ListHosts (110) reply\n");
#endif
  br.skip(br.remaining());

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = 1; // enabled (access control enabled)
    wire::wr32_le(rep.data() + 4, 0); // length = 0 (empty list)
    wire::wr16_le(rep.data() + 8, 0); // nHosts = 0
  });
}

// =============================================================================
// SetAccessControl (111) — void stub
// =============================================================================
void MiscOps::handleSetAccessControl(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  x11_ui_push_log(1, "[MiscOps] SetAccessControl (111) stub\n");
#endif
  br.skip(br.remaining());
}

// =============================================================================
// SetCloseDownMode (112) — void stub
// =============================================================================
void MiscOps::handleSetCloseDownMode(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  x11_ui_push_log(1, "[MiscOps] SetCloseDownMode (112) stub\n");
#endif
  br.skip(br.remaining());
}

// =============================================================================
// KillClient (113) — void stub
// =============================================================================
void MiscOps::handleKillClient(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  x11_ui_push_log(1, "[MiscOps] KillClient (113) stub\n");
#endif
  br.skip(br.remaining());
}

// =============================================================================
// ForceScreenSaver (115) — void stub
// =============================================================================
void MiscOps::handleForceScreenSaver(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  x11_ui_push_log(1, "[MiscOps] ForceScreenSaver (115) stub\n");
#endif
  br.skip(br.remaining());
}

// =============================================================================
// NoOperation (127) — void stub
// =============================================================================
void MiscOps::handleNoOperation(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
#ifdef X11_TRACE_VERBOSE
  x11_ui_push_log(2, "[MiscOps] NoOperation (127)\n");
#endif
  br.skip(br.remaining());
}

} // namespace x11
