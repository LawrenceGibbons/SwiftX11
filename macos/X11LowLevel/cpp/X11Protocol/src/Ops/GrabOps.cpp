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
#include "Utils/MachTime.hpp"
#include "Utils/GrabChoreography.hpp"   // Phase B: activation/deactivation choreography
#include "Core/XProtoServer.hpp"        // eventOps()
#include "Core/timestamp.hpp"           // x11_now_ms_monotonic

extern "C" x11::XProtoServer* x11_proto_bridge_get_server(void);

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

  (void)br.readU32(); // confineTo (not honoured)
  const uint32_t cursor = br.readU32();
  const uint32_t time = br.readU32();

  br.skip(br.remaining());

  // Validate grab window exists (allow root XID 0 and 1)
  if (grabWindow != 0 && grabWindow != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(grabWindow, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, grabWindow, x11::opcode::GrabPointer);
      return;
    }
  }

  // xorg GrabDevice (dix/events.c:5252-5291) status ladder, in its order:
  // AlreadyGrabbed (held by another client, or at the other level) →
  // GrabNotViewable (window not realized) → GrabInvalidTime → activate.
  // Sync modes are accepted but never freeze (G-11: every target client
  // grabs async).  Ownership-aware since review §2.5: Java's liberal
  // XUngrabPointer(CurrentTime) must not stomp another client's menu grab.
  const int      fd  = ctx.transport().clientFd();
  const uint32_t now = x11_now_ms_monotonic();
  PointerGrab held{};
  const bool haveHeld = ctx.grabs().getPointerGrab(held) && held.active;

  uint8_t status = x11::kGrabSuccess;
  if (haveHeld && (held.is_xi2 || (held.owner_fd >= 0 && held.owner_fd != fd))) {
    status = x11::kAlreadyGrabbed;
  } else if (grabWindow != 0 && !grabchoreo::isViewable(ctx, grabWindow)) {
    status = x11::kGrabNotViewable;
  } else if (!x11::grabTimeValid(time, now, haveHeld ? held.grab_time : 0)) {
    status = x11::kGrabInvalidTime;
  } else {
    PointerGrab req{};
    req.grabWindow    = grabWindow;
    req.ownerEvents   = (ownerEvents != 0);
    req.eventMask     = eventMask;
    req.owner_fd      = fd;
    req.grab_time     = time ? time : now;
    req.is_xi2        = false;
    req.pointer_mode  = pointerMode;
    req.keyboard_mode = keyboardMode;
    req.cursor        = cursor;
    status = ctx.grabs().tryPointerGrab(req);
    if (status == x11::kGrabSuccess) {
      // xorg ActivatePointerGrab (dix/events.c:1593-1611): Leave the old grab
      // window (or the sprite window) and Enter the grab window, NotifyGrab,
      // unless the same window was already the grab window.
      if (auto* srv = x11_proto_bridge_get_server()) {
        const uint32_t from = haveHeld ? held.grabWindow : grabchoreo::spriteWindow(ctx);
        if (!(haveHeld && held.grabWindow == grabWindow))
          grabchoreo::pointerGrabCrossings(ctx, srv->eventOps(), from, grabWindow, /*NotifyGrab*/1);
      }
    }
  }

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = status;
  });

#ifndef NDEBUG
  if (status != x11::kGrabSuccess) {
    TS_DBG("[GrabPointer] AlreadyGrabbed: fd=%d wanted 0x%08X\n",
           ctx.transport().clientFd(), (unsigned)grabWindow);
  }
#endif
}

// -----------------------------
// 27 UngrabPointer
// Header: reqType=27, pad(minor), length
// Body (4 bytes): CARD32 time
// -----------------------------
void GrabOps::handleUngrabPointer(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  uint32_t time = 0;
  if (br.remaining() >= 4) time = br.readU32();
  br.skip(br.remaining());

  // xorg ProcUngrabPointer (dix/events.c:5164-5170): deactivate only the
  // caller's own grab (review §2.5 — Java calls XUngrabPointer(CurrentTime)
  // liberally; it must not destroy another client's menu/drag grab) and
  // only when the time is within [grab time, now] (M22).
  PointerGrab held{};
  if (!(ctx.grabs().getPointerGrab(held) && held.active)) return;
  const int fd = ctx.transport().clientFd();
  if (held.owner_fd >= 0 && held.owner_fd != fd) return;
  if (!x11::ungrabTimeValid(time, x11_now_ms_monotonic(), held.grab_time)) return;

  ctx.grabs().clearPointerGrab(fd);

  // xorg DeactivatePointerGrab (dix/events.c:1670, 1688-1689): the grab is
  // gone first, then Leave(grab window) / Enter(sprite window), NotifyUngrab.
  if (auto* srv = x11_proto_bridge_get_server()) {
    grabchoreo::pointerGrabCrossings(ctx, srv->eventOps(), held.grabWindow,
                                     grabchoreo::spriteWindow(ctx), /*NotifyUngrab*/2);
  }
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
void GrabOps::handleGrabKeyboard(XProtoContext& ctx, uint16_t seq, uint8_t ownerEvents, ByteReader& br) {
  // Body (12 bytes): grabWindow(4), time(4), pointerMode(1), keyboardMode(1), pad(2)
  uint32_t grabWindow = 0, time = 0;
  if (br.remaining() >= 4) grabWindow = br.readU32();
  if (br.remaining() >= 4) time = br.readU32();
  br.skip(br.remaining());

  // Validate grab window exists (allow root XID 0 and 1)
  if (grabWindow != 0 && grabWindow != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(grabWindow, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, grabWindow, x11::opcode::GrabKeyboard);
      return;
    }
  }

  // xorg GrabDevice status ladder (dix/events.c:5252-5266), as for the
  // pointer.  Ownership-aware since review §2.5.
  const int      fd  = ctx.transport().clientFd();
  const uint32_t now = x11_now_ms_monotonic();
  KeyboardGrab held{};
  const bool haveHeld = ctx.grabs().getKeyboardGrabInfo(held) && held.active;

  uint8_t status = x11::kGrabSuccess;
  if (haveHeld && (held.is_xi2 || (held.owner_fd >= 0 && held.owner_fd != fd))) {
    status = x11::kAlreadyGrabbed;
  } else if (grabWindow != 0 && !grabchoreo::isViewable(ctx, grabWindow)) {
    status = x11::kGrabNotViewable;
  } else if (!x11::grabTimeValid(time, now, haveHeld ? held.grab_time : 0)) {
    status = x11::kGrabInvalidTime;
  } else {
    KeyboardGrab req{};
    req.grabWindow  = grabWindow;
    req.ownerEvents = (ownerEvents != 0);
    req.owner_fd    = fd;
    req.grab_time   = time ? time : now;
    req.is_xi2      = false;
    status = ctx.grabs().tryKeyboardGrab(req);
    if (status == x11::kGrabSuccess) {
      // xorg ActivateKeyboardGrab (dix/events.c:1720-1735): FocusOut(old
      // grab window, else the focus window) / FocusIn(grab window) with
      // NotifyGrab at both levels, unless the same window was already the
      // grab window.  Java AWT depends on this pair to proceed with
      // clipboard operations after menu dismissal.
      if (auto* srv = x11_proto_bridge_get_server()) {
        uint32_t from = haveHeld ? held.grabWindow : ctx.input().focus_xid;
        if (!from) from = grabchoreo::spriteWindow(ctx);
        if (!(haveHeld && held.grabWindow == grabWindow))
          grabchoreo::keyboardGrabFocusPair(ctx, srv->eventOps(), from, grabWindow, /*NotifyGrab*/1);
      }
    }
  }

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = status;
  });
}

// 32 UngrabKeyboard (void)
// X11 spec: When the keyboard grab is released, the server generates
// FocusOut(mode=Ungrab) to the grab window and FocusIn(mode=Ungrab)
// to the focus window. Java AWT depends on these events to proceed
// with clipboard operations after menu dismissal.
void GrabOps::handleUngrabKeyboard(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  uint32_t time = 0;
  if (br.remaining() >= 4) time = br.readU32();
  br.skip(br.remaining());

  // xorg ProcUngrabKeyboard (dix/events.c:5356-5360): only the caller's own
  // grab, only within [grab time, now] (M22).
  KeyboardGrab held{};
  if (!(ctx.grabs().getKeyboardGrabInfo(held) && held.active)) return;
  const int fd = ctx.transport().clientFd();
  if (held.owner_fd >= 0 && held.owner_fd != fd) return;
  if (!x11::ungrabTimeValid(time, x11_now_ms_monotonic(), held.grab_time)) return;

  const uint32_t grabWin = ctx.grabs().clearKeyboardGrab(fd);
  if (!grabWin) return;

  // xorg DeactivateKeyboardGrab (dix/events.c:1772-1782): FocusOut(grab
  // window) / FocusIn(focus window, else sprite window), NotifyUngrab, at
  // both levels.
  if (auto* srv = x11_proto_bridge_get_server()) {
    uint32_t to = ctx.input().focus_xid;
    if (!to) to = grabchoreo::spriteWindow(ctx);
    grabchoreo::keyboardGrabFocusPair(ctx, srv->eventOps(), grabWin, to, /*NotifyUngrab*/2);
  }
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

// -----------------------------
// 30 ChangeActivePointerGrab (void)
// Body (12 bytes): CARD32 cursor, CARD32 time, CARD16 eventMask, CARD16 pad
// -----------------------------
void GrabOps::handleChangeActivePointerGrab(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }
  (void)br.readU32(); // cursor (grab cursors are not applied yet — L15)
  const uint32_t time = br.readU32();
  const uint16_t eventMask = br.readU16();
  (void)br.readU16(); // pad
  br.skip(br.remaining());

  // xorg ProcChangeActivePointerGrab (dix/events.c:5127-5145): only the
  // caller's own grab, only within [grab time, now] (M22 — a foreign client
  // could previously rewrite the active grab's mask).
  PointerGrab held{};
  if (!(ctx.grabs().getPointerGrab(held) && held.active)) return;
  if (held.owner_fd >= 0 && held.owner_fd != ctx.transport().clientFd()) return;
  if (!x11::ungrabTimeValid(time, x11_now_ms_monotonic(), held.grab_time)) return;
  ctx.grabs().updatePointerGrabEventMask(eventMask);
}

} // namespace x11
