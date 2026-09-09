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
#include "Core/CursorRouting.hpp"       // maybeApplyCursor — grab cursor (L15)
#include "Transport/XProtoDaemon.hpp"   // grabServer/ungrabServer (C8)

extern "C" x11::XProtoServer* x11_proto_bridge_get_server(void);
namespace x11 { class XProtoDaemon; }
extern "C" x11::XProtoDaemon* x11_proto_bridge_get_daemon(void);   // C8: GrabServer

namespace x11 {

// xorg PostNewCursor after a grab is activated, changed or released
// (dix/events.c:1620, 1690, ProcChangeActivePointerGrab): re-evaluate the
// cursor for the host the pointer is in — maybeApplyCursor prefers an
// active grab's cursor (L15).
static void reapplyPointerCursor(XProtoContext& ctx) {
  const uint32_t host = ctx.input().last_xid;
  if (!host) return;
  maybeApplyCursor(ctx, host, ctx.input().routePointer(ctx.input().pointer_xid));
}

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

#ifndef NDEBUG
  if (pointerMode == 0 || keyboardMode == 0)   // GrabModeSync — accepted, never frozen (L23)
    TS_FPRINTF("[GRAB_SYNC] GrabPointer fd=%d win=0x%08X pointer_mode=%u keyboard_mode=%u\n",
               ctx.transport().clientFd(), (unsigned)grabWindow,
               (unsigned)pointerMode, (unsigned)keyboardMode);
#endif

  // Validate grab window exists (allow root XID 0 and 1)
  if (grabWindow != 0 && grabWindow != x11::kRootWindowXid) {
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
      reapplyPointerCursor(ctx);   // the grab cursor shows at once (L15)
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
  reapplyPointerCursor(ctx);   // window cursor back (L15)
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
  if (grabWindow != 0 && grabWindow != x11::kRootWindowXid) {
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
  g.owner_fd = ctx.transport().clientFd();   // C1: rClient(grab)

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
  if (grabWindow != 0 && grabWindow != x11::kRootWindowXid) {
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
  uint8_t pointerMode = 1, keyboardMode = 1;
  if (br.remaining() >= 4) grabWindow = br.readU32();
  if (br.remaining() >= 4) time = br.readU32();
  if (br.remaining() >= 2) { pointerMode = br.readU8(); keyboardMode = br.readU8(); }
  br.skip(br.remaining());
#ifndef NDEBUG
  if (pointerMode == 0 || keyboardMode == 0)   // GrabModeSync — accepted, never frozen (L23)
    TS_FPRINTF("[GRAB_SYNC] GrabKeyboard fd=%d win=0x%08X pointer_mode=%u keyboard_mode=%u\n",
               ctx.transport().clientFd(), (unsigned)grabWindow,
               (unsigned)pointerMode, (unsigned)keyboardMode);
#endif

  // Validate grab window exists (allow root XID 0 and 1)
  if (grabWindow != 0 && grabWindow != x11::kRootWindowXid) {
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
      // xorg ActivateKeyboardGrab (dix/events.c:1720-1735): DoFocusEvents(old
      // grab window, else the focus window — None sends nothing, PointerRoot
      // is a window here; never the sprite window for a master keyboard —
      // → grab window, NotifyGrab) at both levels, unless the same window
      // was already the grab window (:1732-1734).  Java AWT depends on this
      // pair to proceed with clipboard operations after menu dismissal.
      if (auto* srv = x11_proto_bridge_get_server()) {
        const uint32_t from = haveHeld ? held.grabWindow : ctx.input().focus_xid;
        if (from && !(haveHeld && held.grabWindow == grabWindow))
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

  // xorg DeactivateKeyboardGrab (dix/events.c:1772-1782): DoFocusEvents(grab
  // window → focus window, NotifyUngrab) at both levels; a None / PointerRoot
  // focus yields just the FocusOut side (CoreFocusToPointerRootOrNone).
  if (auto* srv = x11_proto_bridge_get_server()) {
    grabchoreo::keyboardGrabFocusPair(ctx, srv->eventOps(), grabWin, ctx.input().focus_xid,
                                      /*NotifyUngrab*/2);
  }
}

// 33 GrabKey (void) — C2 passive keyboard grab.
// Body (12 bytes): grabWindow(4), modifiers(2), key(1), pointerMode(1),
//                  keyboardMode(1), pad(3).
void GrabOps::handleGrabKey(XProtoContext& ctx, uint16_t seq, uint8_t ownerEvents, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }
  const uint32_t grabWindow = br.readU32();
  const uint16_t modifiers  = br.readU16();
  const uint8_t  key        = br.readU8();
  const uint8_t  pointerMode  = br.readU8();
  const uint8_t  keyboardMode = br.readU8();
  (void)pointerMode; (void)keyboardMode;   // sync modes accepted, never frozen (L23)
  br.skip(br.remaining());

  // Validate grab window exists (allow root XID 0 and 1).
  if (grabWindow != 0 && grabWindow != x11::kRootWindowXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(grabWindow, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, grabWindow, x11::opcode::GrabKey);
      return;
    }
  }
  // xorg ProcGrabKey (dix/events.c): a keycode other than AnyKey must lie in
  // [min_keycode, max_keycode]; ours is [8, 255].  BadValue otherwise.
  if (key != x11::AnyKey && key < 8) {
    ctx.transport().sendErrorCore(x11::error::BadValue, seq, key, x11::opcode::GrabKey);
    return;
  }

  PassiveKeyGrab g{};
  g.grabWindow  = grabWindow;
  g.key         = key;                 // 0 => AnyKey
  g.modifiers   = modifiers;           // 0x8000 => AnyModifier
  g.ownerEvents = (ownerEvents != 0);
  g.owner_fd    = ctx.transport().clientFd();   // rClient(grab)
  ctx.grabs().addOrReplaceKey(g);
}

// 34 UngrabKey (void).
// Header: reqType=34, key=minor, length.  Body (8 bytes): grabWindow(4),
//         modifiers(2), pad(2).
void GrabOps::handleUngrabKey(XProtoContext& ctx, uint16_t seq, uint8_t key, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }
  const uint32_t grabWindow = br.readU32();
  const uint16_t modifiers  = br.readU16();
  (void)br.readU16(); // pad
  br.skip(br.remaining());

  if (grabWindow != 0 && grabWindow != x11::kRootWindowXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(grabWindow, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, grabWindow, x11::opcode::UngrabKey);
      return;
    }
  }
  ctx.grabs().removeKey(grabWindow, key, modifiers);
}

// 35 AllowEvents (void).  Sync grab modes never freeze here (every target
// client grabs async), so there is nothing to thaw; the mode is logged so a
// sync user shows up in the trace (L23).
void GrabOps::handleAllowEvents(XProtoContext& ctx, uint16_t /*seq*/, uint8_t mode, ByteReader& br) {
  br.skip(br.remaining());
#ifndef NDEBUG
  TS_FPRINTF("[GRAB_SYNC] AllowEvents fd=%d mode=%u (no frozen device to thaw)\n",
             ctx.transport().clientFd(), (unsigned)mode);
#else
  (void)ctx; (void)mode;
#endif
}

// 36 GrabServer (void) — C8.  Suspend all other clients until UngrabServer:
// the daemon poll loop stops servicing every fd but this one (xorg
// ProcGrabServer → OnlyListenToOneClient, dix/dispatch.c:1156-1173).  Java AWT
// brackets focus/restack and selection setup in XGrabServer/XUngrabServer and
// relied on nothing else interleaving.
void GrabOps::handleGrabServer(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  br.skip(br.remaining());
  if (auto* d = x11_proto_bridge_get_daemon())
    d->grabServer(ctx.transport().clientFd());
}

// 37 UngrabServer (void) — release the server grab held by this client; the
// poll loop resumes all clients next iteration (xorg ProcUngrabServer →
// ListenToAllClients, dix/dispatch.c:1187-1204).
void GrabOps::handleUngrabServer(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  br.skip(br.remaining());
  if (auto* d = x11_proto_bridge_get_daemon())
    d->ungrabServer(ctx.transport().clientFd());
}

// -----------------------------
// 30 ChangeActivePointerGrab (void)
// Body (12 bytes): CARD32 cursor, CARD32 time, CARD16 eventMask, CARD16 pad
// -----------------------------
void GrabOps::handleChangeActivePointerGrab(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }
  const uint32_t cursor = br.readU32();
  const uint32_t time = br.readU32();
  const uint16_t eventMask = br.readU16();
  (void)br.readU16(); // pad
  br.skip(br.remaining());

  // xorg ProcChangeActivePointerGrab (dix/events.c:5127-5145): only the
  // caller's own grab, only within [grab time, now] (M22 — a foreign client
  // could previously rewrite the active grab's mask); the new cursor is
  // posted at once (PostNewCursor) — L15.
  PointerGrab held{};
  if (!(ctx.grabs().getPointerGrab(held) && held.active)) return;
  if (held.owner_fd >= 0 && held.owner_fd != ctx.transport().clientFd()) return;
  if (!x11::ungrabTimeValid(time, x11_now_ms_monotonic(), held.grab_time)) return;
  ctx.grabs().updatePointerGrabEventMask(eventMask);
  ctx.grabs().updatePointerGrabCursor(cursor);
  reapplyPointerCursor(ctx);
}

} // namespace x11
