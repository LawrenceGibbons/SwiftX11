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

namespace x11 {

GrabOps::GrabOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::GrabPointer,   &GrabOps::onMajor, this); // 26
  reg.registerMajor(x11::opcode::UngrabPointer, &GrabOps::onMajor, this); // 27
  reg.registerMajor(x11::opcode::GrabButton,    &GrabOps::onMajor, this); // 28
  reg.registerMajor(x11::opcode::UngrabButton,  &GrabOps::onMajor, this); // 29
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
void GrabOps::handleGrabPointer(XProtoContext& ctx, uint16_t /*seq*/, uint8_t ownerEvents, ByteReader& br) {
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

#ifndef NDEBUG
  ctx.tracef("[GrabOps] GrabPointer win=0x%08X owner=%u mask=0x%04X\n",
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
  ctx.tracef("[GrabOps] UngrabPointer\n");
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

} // namespace x11
