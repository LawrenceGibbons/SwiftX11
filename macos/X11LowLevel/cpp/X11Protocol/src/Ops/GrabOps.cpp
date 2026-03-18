//
//  GrabOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/17/26.
//

#include "Ops/GrabOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include "Core/XConstants.hpp"
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
  reg.registerMajor(x11::opcode::ChangeActivePointerGrab, &GrabOps::onMajor, this); // 30
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
    case x11::opcode::ChangeActivePointerGrab: handleChangeActivePointerGrab(ctx, dc.seq, dc.br); return;
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

  // Validate grab window exists (allow root XID 0 and 1)
  if (grabWindow != 0 && grabWindow != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(grabWindow, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, grabWindow, x11::opcode::GrabPointer);
      return;
    }
  }

  ctx.grabs().setPointerGrab(grabWindow, ownerEvents != 0, eventMask);

  // GrabPointer reply: status=GrabSuccess (same pattern as GrabKeyboard).
  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = 0; // GrabSuccess
  });

}

// -----------------------------
// 27 UngrabPointer
// Header: reqType=27, pad(minor), length
// Body (4 bytes): CARD32 time
// -----------------------------
void GrabOps::handleUngrabPointer(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() >= 4) (void)br.readU32(); // time
  br.skip(br.remaining());

  ctx.grabs().clearPointerGrab();
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
void GrabOps::handleGrabButton(XProtoContext& ctx, uint16_t seq, uint8_t ownerEvents, ByteReader& br) {
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

  // Validate grab window exists (allow root XID 0 and 1)
  if (grabWindow != 0 && grabWindow != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(grabWindow, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, grabWindow, x11::opcode::GrabButton);
      return;
    }
  }

  PassiveGrab g{};
  g.grabWindow = grabWindow;
  g.button = button;                 // 0 => AnyButton
  g.modifiers = modifiers;           // 0x8000 => AnyModifier
  g.ownerEvents = (ownerEvents != 0);
  g.eventMask = eventMask;

  ctx.grabs().addOrReplace(g);
}

// -----------------------------
// 29 UngrabButton
// Header: reqType=29, button=minor, length
// Body (8 bytes):
//   CARD32 grabWindow
//   CARD16 modifiers
//   CARD16 pad
// -----------------------------
void GrabOps::handleUngrabButton(XProtoContext& ctx, uint16_t seq, uint8_t button, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }

  const uint32_t grabWindow = br.readU32();
  const uint16_t modifiers  = br.readU16();
  (void)br.readU16(); // pad

  br.skip(br.remaining());

  // Validate grab window exists (allow root XID 0 and 1)
  if (grabWindow != 0 && grabWindow != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(grabWindow, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, grabWindow, x11::opcode::UngrabButton);
      return;
    }
  }

  ctx.grabs().remove(grabWindow, button, modifiers);

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
  // Body (12 bytes): grabWindow(4), time(4), pointerMode(1), keyboardMode(1), pad(2)
  uint32_t grabWindow = 0;
  if (br.remaining() >= 4) grabWindow = br.readU32();
  br.skip(br.remaining());

  // Validate grab window exists (allow root XID 0 and 1)
  if (grabWindow != 0 && grabWindow != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(grabWindow, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, grabWindow, x11::opcode::GrabKeyboard);
      return;
    }
  }

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
void GrabOps::handleGrabServer(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  br.skip(br.remaining());
#ifndef NDEBUG
  fprintf(stderr, "[GrabServer] seq=%u fd=%d\n", (unsigned)seq, ctx.transport().clientFd());
#endif
}

// 37 UngrabServer (void, no-op)
void GrabOps::handleUngrabServer(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  br.skip(br.remaining());
#ifndef NDEBUG
  fprintf(stderr, "[UngrabServer] seq=%u fd=%d\n", (unsigned)seq, ctx.transport().clientFd());
#endif
}

// -----------------------------
// 30 ChangeActivePointerGrab (void)
// Body (12 bytes): CARD32 cursor, CARD32 time, CARD16 eventMask, CARD16 pad
// -----------------------------
void GrabOps::handleChangeActivePointerGrab(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }
  (void)br.readU32(); // cursor
  (void)br.readU32(); // time
  const uint16_t eventMask = br.readU16();
  (void)br.readU16(); // pad
  br.skip(br.remaining());

  // Update the active pointer grab's event mask if a grab is active
  ctx.grabs().updatePointerGrabEventMask(eventMask);
}

} // namespace x11
