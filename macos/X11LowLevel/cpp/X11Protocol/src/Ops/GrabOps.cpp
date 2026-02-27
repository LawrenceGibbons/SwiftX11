//
//  GrabOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/17/26.
//

#include "Ops/GrabOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Utils/ByteReader.hpp"
#include "Core/GrabTable.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/WireLE.hpp"

namespace x11 {

GrabOps::GrabOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::GrabPointer,   &GrabOps::onMajor, this); // 26
  reg.registerMajor(x11::opcode::UngrabPointer, &GrabOps::onMajor, this); // 27
  reg.registerMajor(x11::opcode::GrabButton,    &GrabOps::onMajor, this); // 28
  reg.registerMajor(x11::opcode::UngrabButton,  &GrabOps::onMajor, this); // 29
  reg.registerMajor(x11::opcode::GrabKeyboard,  &GrabOps::onMajor, this); // 31
  reg.registerMajor(x11::opcode::UngrabKeyboard,&GrabOps::onMajor, this); // 32
  reg.registerMajor(x11::opcode::GrabKey,       &GrabOps::onMajor, this); // 33
  reg.registerMajor(x11::opcode::UngrabKey,     &GrabOps::onMajor, this); // 34
  reg.registerMajor(x11::opcode::AllowEvents,   &GrabOps::onMajor, this); // 35
  reg.registerMajor(x11::opcode::GrabServer,    &GrabOps::onMajor, this); // 36
  reg.registerMajor(x11::opcode::UngrabServer,  &GrabOps::onMajor, this); // 37
}

void GrabOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<GrabOps*>(user)->handle(ctx, dc);
}

void GrabOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::GrabPointer:   handleGrabPointer(ctx, dc.seq, dc.minor, dc.br); return;
    case x11::opcode::UngrabPointer: handleUngrabPointer(ctx, dc.seq, dc.br); return;
    case x11::opcode::GrabButton:    handleGrabButton(ctx, dc.seq, dc.minor, dc.br); return;
    case x11::opcode::UngrabButton:  handleUngrabButton(ctx, dc.seq, dc.minor, dc.br); return;
    case x11::opcode::GrabKeyboard:  handleGrabKeyboard(ctx, dc.seq, dc.minor, dc.br); return;
    case x11::opcode::UngrabKeyboard:handleUngrabKeyboard(ctx, dc.seq, dc.br); return;
    case x11::opcode::GrabKey:       handleGrabKey(ctx, dc.seq, dc.minor, dc.br); return;
    case x11::opcode::UngrabKey:     handleUngrabKey(ctx, dc.seq, dc.minor, dc.br); return;
    case x11::opcode::AllowEvents:   handleAllowEvents(ctx, dc.seq, dc.minor, dc.br); return;
    case x11::opcode::GrabServer:    handleGrabServer(ctx, dc.seq, dc.br); return;
    case x11::opcode::UngrabServer:  handleUngrabServer(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      return;
  }
}

// -----------------------------
// 26 GrabPointer
// Header:  reqType=26, ownerEvents=minor, length
// Body (20 bytes):
//   CARD32 grabWindow
//   CARD16 eventMask
//   BYTE   pointerMode
//   BYTE   keyboardMode
//   CARD32 confineTo
//   CARD32 cursor
//   CARD32 time
// -----------------------------
void GrabOps::handleGrabPointer(XProtoContext& ctx, uint16_t seq, uint8_t ownerEvents, ByteReader& br) {
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t grabWindow = br.readU32();
  const uint16_t eventMask  = br.readU16();
  const uint8_t  pointerMode  = br.readU8();
  const uint8_t  keyboardMode = br.readU8();
  (void)pointerMode; (void)keyboardMode;

  (void)br.readU32(); // confineTo
  (void)br.readU32(); // cursor
  (void)br.readU32(); // time

  br.skip(br.remaining());

  GrabTable::instance().setPointerGrab(grabWindow, ownerEvents != 0, eventMask);

  // GrabPointer REQUIRES a reply (status=GrabSuccess).
  // Without this, the client blocks forever waiting for the reply.
  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = 0; // GrabSuccess
  });

#ifndef NDEBUG
  fprintf(stderr, "[GrabPointer] win=0x%08X owner=%u mask=0x%04X\n",
          (unsigned)grabWindow, (unsigned)ownerEvents, (unsigned)eventMask);
#endif
}

// -----------------------------
// 27 UngrabPointer
// Header: reqType=27, pad(minor), length
// Body (4 bytes): CARD32 time
// -----------------------------
void GrabOps::handleUngrabPointer(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() >= 4) (void)br.readU32(); // time
  br.skip(br.remaining());

  GrabTable::instance().clearPointerGrab();

#ifndef NDEBUG
  fprintf(stderr, "[UngrabPointer]\n");
#endif
}

// -----------------------------
// 28 GrabButton
// Header: reqType=28, ownerEvents=minor, length
// Body (20 bytes):
//   CARD32 grabWindow
//   CARD16 eventMask
//   BYTE   pointerMode
//   BYTE   keyboardMode
//   CARD32 confineTo
//   CARD32 cursor
//   BYTE   button
//   BYTE   pad1
//   CARD16 modifiers
// -----------------------------
void GrabOps::handleGrabButton(XProtoContext& ctx, uint16_t /*seq*/, uint8_t ownerEvents, ByteReader& br) {
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t grabWindow = br.readU32();
  const uint16_t eventMask  = br.readU16();
  const uint8_t  pointerMode  = br.readU8();
  const uint8_t  keyboardMode = br.readU8();
  (void)pointerMode; (void)keyboardMode;

  (void)br.readU32(); // confineTo
  (void)br.readU32(); // cursor

  const uint8_t button = br.readU8();
  (void)br.readU8(); // pad1
  const uint16_t modifiers = br.readU16();

  br.skip(br.remaining());

  PassiveGrab g{};
  g.grabWindow = grabWindow;
  g.button = button;                 // 0 => AnyButton
  g.modifiers = modifiers;           // 0x8000 => AnyModifier
  g.ownerEvents = (ownerEvents != 0);
  g.eventMask = eventMask;

  GrabTable::instance().addOrReplace(g);

#ifndef NDEBUG
  ctx.tracef("[GrabOps] GrabButton win=0x%08X btn=%u mods=0x%04X owner=%u mask=0x%04X\n",
             (unsigned)grabWindow, (unsigned)button, (unsigned)modifiers,
             (unsigned)ownerEvents, (unsigned)eventMask);
#endif
}

// -----------------------------
// 29 UngrabButton
// Header: reqType=29, button=minor, length
// Body (8 bytes):
//   CARD32 grabWindow
//   CARD16 modifiers
//   CARD16 pad
// -----------------------------
void GrabOps::handleUngrabButton(XProtoContext& ctx, uint16_t /*seq*/, uint8_t button, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }

  const uint32_t grabWindow = br.readU32();
  const uint16_t modifiers  = br.readU16();
  (void)br.readU16(); // pad

  br.skip(br.remaining());

  GrabTable::instance().remove(grabWindow, button, modifiers);

#ifndef NDEBUG
  ctx.tracef("[GrabOps] UngrabButton win=0x%08X btn=%u mods=0x%04X\n",
             (unsigned)grabWindow, (unsigned)button, (unsigned)modifiers);
#endif
}

// -----------------------------
// 31 GrabKeyboard (reply: status=GrabSuccess)
// Body (12 bytes): grabWindow(4), time(4), pointerMode(1), keyboardMode(1), pad(2)
// -----------------------------
void GrabOps::handleGrabKeyboard(XProtoContext& ctx, uint16_t seq, uint8_t /*ownerEvents*/, ByteReader& br) {
  br.skip(br.remaining());
  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = 0; // GrabSuccess
  });
}

// 32 UngrabKeyboard (void)
void GrabOps::handleUngrabKeyboard(XProtoContext& /*ctx*/, uint16_t /*seq*/, ByteReader& br) {
  br.skip(br.remaining());
}

// 33 GrabKey (void)
void GrabOps::handleGrabKey(XProtoContext& /*ctx*/, uint16_t /*seq*/, uint8_t /*ownerEvents*/, ByteReader& br) {
  br.skip(br.remaining());
}

// 34 UngrabKey (void)
void GrabOps::handleUngrabKey(XProtoContext& /*ctx*/, uint16_t /*seq*/, uint8_t /*keycode*/, ByteReader& br) {
  br.skip(br.remaining());
}

// 35 AllowEvents (void)
void GrabOps::handleAllowEvents(XProtoContext& /*ctx*/, uint16_t /*seq*/, uint8_t /*mode*/, ByteReader& br) {
  br.skip(br.remaining());
}

// 36 GrabServer (void, no-op for single-process)
void GrabOps::handleGrabServer(XProtoContext& /*ctx*/, uint16_t /*seq*/, ByteReader& br) {
  br.skip(br.remaining());
}

// 37 UngrabServer (void, no-op)
void GrabOps::handleUngrabServer(XProtoContext& /*ctx*/, uint16_t /*seq*/, ByteReader& br) {
  br.skip(br.remaining());
}

} // namespace x11
