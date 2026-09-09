//
//  XKBOps.cpp
//  X11LowLevel
//
//  XKEYBOARD extension (major opcode 145).  Mirrors xorg xkb/xkb.c for the
//  client-info surface the clients we host actually use:
//
//    libX11 _XkbLoadDpy      UseExtension, GetMap(full=0x07), SelectEvents ×2
//    GDK 3.24                + SelectEvents(State details), PerClientFlags,
//                              GetMap(full=0x47), GetNames(0x1800), GetState,
//                              GetControls, Bell
//    Java AWT                  GetMap(full=0x47), PerClientFlags, SelectEvents
//    Chromium 124              GetMap(full=0x03), PerClientFlags, SelectEvents
//    libxkbcommon-x11 1.0.3    GetDeviceInfo, GetMap(full=0xDF), GetIndicatorMap,
//                              GetCompatMap, GetNames(0x1FF5), GetControls, GetState
//    xkbcomp / setxkbmap       GetKbdByName
//
//  Every request except UseExtension is refused with BadAccess until the
//  client has done UseExtension (xorg `_XkbClientInitialized`).  Requests we
//  do not model (SetMap, SetControls, …) are consumed as void no-ops; every
//  reply-bearing request always answers, so XCB never desyncs.
//

#include "Extensions/XKBOps.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Core/XProtoContext.hpp"
#include "Core/XClient.hpp"
#include "Core/XConstants.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/X11ExtOpcodes.hpp"
#include "Core/X11Modifiers.hpp"
#include "Core/InputState.hpp"
#include "Core/AtomTable.hpp"
#include "Core/PropertyTable.hpp"
#include "Core/XkbKeymap.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Utils/ByteReader.hpp"
#include "Utils/WireLE.hpp"
#include "Utils/MachTime.hpp"

extern "C" {
#include "SwiftX11Bridge.h"
}

namespace x11 {

namespace {

using namespace x11::xkb;

// XKB minor opcodes (XKB.h X_kb*)
enum : uint8_t {
  kUseExtension      = 0,
  kSelectEvents      = 1,
  kBell              = 3,
  kGetState          = 4,
  kLatchLockState    = 5,
  kGetControls       = 6,
  kSetControls       = 7,
  kGetMap            = 8,
  kSetMap            = 9,
  kGetCompatMap      = 10,
  kSetCompatMap      = 11,
  kGetIndicatorState = 12,
  kGetIndicatorMap   = 13,
  kSetIndicatorMap   = 14,
  kGetNamedIndicator = 15,
  kSetNamedIndicator = 16,
  kGetNames          = 17,
  kSetNames          = 18,
  kGetGeometry       = 19,
  kSetGeometry       = 20,
  kPerClientFlags    = 21,
  kListComponents    = 22,
  kGetKbdByName      = 23,
  kGetDeviceInfo     = 24,
  kSetDeviceInfo     = 25,
  kSetDebuggingFlags = 101,
};

// xorg XkbErr_* sub-codes for the BadKeyboard error value
constexpr uint8_t kErrBadDevice = 0xff;

inline uint32_t errCode2(uint8_t a, uint32_t b) { return (uint32_t(a) << 24) | (b & 0xffffffu); }

void sendError(XProtoContext& ctx, uint8_t code, uint16_t seq, uint32_t value, uint8_t minor) {
  (void)ctx.transport().sendErrorExt(code, seq, value, minor, ext::kXKB);
}

void sendBadKeyboard(XProtoContext& ctx, uint16_t seq, uint16_t spec, uint8_t minor) {
  sendError(ctx, ext::kXKB_FirstError, seq, errCode2(kErrBadDevice, spec), minor);
}

// CHK_KBD_DEVICE / CHK_ANY_DEVICE: XkbUseCoreKbd and our XI2 keyboard ids
// (master 3, XTEST 5, real slave 7) are keyboards; the pointer ids only pass
// where xorg accepts any device.  Replies carry the resolved id, and clients
// (libX11 after its first GetMap, libxkbcommon after GetDeviceInfo) send it
// back as deviceSpec.
bool resolveDevice(uint16_t spec, bool keyboardOnly, uint8_t& id) {
  switch (spec) {
    case kUseCoreKbd: id = kCoreKeyboardId; return true;
    case 3: case 5: case 7: id = (uint8_t)spec; return true;
    case kUseCorePtr: if (keyboardOnly) return false; id = kCorePointerId; return true;
    case 2: case 4: case 6: if (keyboardOnly) return false; id = (uint8_t)spec; return true;
    default: return false;
  }
}

bool isKeyboardId(uint8_t id) { return id == 3 || id == 5 || id == 7; }

const char* deviceName(uint8_t id) {
  switch (id) {
    case 2: return "Virtual core pointer";
    case 3: return "Virtual core keyboard";
    case 4: return "Virtual core XTEST pointer";
    case 5: return "Virtual core XTEST keyboard";
    case 6: return "SwiftX11 pointer";
    case 7: return "SwiftX11 keyboard";
    default: return "";
  }
}

// Effective real modifiers (core state byte) and lock state from InputState.
uint8_t currentMods(XProtoContext& ctx) {
  return (uint8_t)(x11::input::toX11State(0, ctx.input().mods) & 0xffu);
}
bool capsLockOn(XProtoContext& ctx) {
  return (ctx.input().mods & x11::input::Lock) != 0;
}
uint16_t currentButtonState(XProtoContext& ctx) {
  return (uint16_t)(x11::input::toX11State(ctx.input().buttons, 0) & 0x1f00u);
}

void sendVector(XProtoContext& ctx, const std::vector<uint8_t>& v) {
  (void)ctx.reply().sendReplyRaw(v.data(), v.size());
}

// xorg publishes the RMLVO names on the root window so `setxkbmap -query`
// has something to print.  Written once, when the first client initialises.
void ensureRulesNamesProperty() {
  static bool s_done = false;
  if (s_done) return;
  s_done = true;
  // rules \0 model \0 layout \0 variant \0 options \0
  // `setxkbmap -query` loads the named rules file from the xkb data dir, so
  // the rules must be a real file (evdev ships with xkeyboard-config on
  // every client); the model/layout are the closest stock description of
  // what we serve.  SetMap is a no-op, so a client compiling a keymap from
  // these names cannot change anything.
  static const char rmlvo[] = "evdev\0pc105\0us\0\0";
  const uint32_t atom = AtomTable::instance().intern("_XKB_RULES_NAMES", 16, false);
  PropertyTable::instance().setReplace(kRootWindowXid, atom, /*XA_STRING*/ 31, 8,
                                       reinterpret_cast<const uint8_t*>(rmlvo), sizeof(rmlvo));
}

void logOnce(uint8_t minor, const char* what) {
  static uint32_t s_seen[4] = {0, 0, 0, 0};
  const unsigned idx = minor & 0x7f;
  if (s_seen[idx >> 5] & (1u << (idx & 31))) return;
  s_seen[idx >> 5] |= (1u << (idx & 31));
  char buf[160];
  snprintf(buf, sizeof(buf), "[XKB] %s (minor %u) accepted as a no-op — not modelled\n",
           what, (unsigned)minor);
  x11_ui_push_log(1, buf);
}

} // namespace

// ============================================================================
// Dispatch
// ============================================================================
void XKBOps::dispatch(XProtoContext& ctx, DispatchContext& dc)
{
  const uint8_t  minor = dc.minor;
  const uint16_t seq   = dc.seq;
  ByteReader&    br    = dc.br;

  XClient* cl = ctx.hasClient() ? ctx.client() : nullptr;
  if (!cl) {
    br.skip(br.remaining());
    sendError(ctx, x11::error::BadAccess, seq, 0, minor);
    return;
  }
  XkbClientState& st = cl->xkb();

#ifndef NDEBUG
  TS_FPRINTF("[XKB] fd=%d minor=%u seq=%u body=%zu init=%d\n",
             ctx.transport().clientFd(), (unsigned)minor, (unsigned)seq,
             br.remaining(), (int)st.initialised);
#endif

  // xorg: every request but UseExtension needs _XkbClientInitialized.
  if (minor != kUseExtension && !st.initialised) {
    br.skip(br.remaining());
    sendError(ctx, x11::error::BadAccess, seq, 0, minor);
    return;
  }

  // Request bodies are fixed-size; a short one is BadLength (REQUEST_SIZE_MATCH).
  auto need = [&](size_t n) -> bool {
    if (br.remaining() >= n) return true;
    br.skip(br.remaining());
    sendError(ctx, x11::error::BadLength, seq, 0, minor);
    return false;
  };

  const Keymap& km = Keymap::current();

  switch (minor) {

  // ---- 0: UseExtension (reply) ----
  case kUseExtension: {
    if (!need(4)) return;
    const uint16_t wantedMajor = br.readU16();
    const uint16_t wantedMinor = br.readU16();
    br.skip(br.remaining());
    // xorg: supported iff wantedMajor == 1 (any minor), or the legacy 0.65.
    const bool supported = (wantedMajor == 1) || (wantedMajor == 0 && wantedMinor == 65);
    if (supported) {
      st.initialised = true;
      ensureRulesNamesProperty();
    }
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = supported ? 1 : 0;
      wire::wr16_le(rep.data() + 8, 1);    // serverMajor
      wire::wr16_le(rep.data() + 10, 0);   // serverMinor
    });
    return;
  }

  // ---- 1: SelectEvents (void) ----
  case kSelectEvents: {
    if (!need(12)) return;
    const uint16_t deviceSpec  = br.readU16();
    const uint16_t affectWhich = br.readU16();
    const uint16_t clear       = br.readU16();
    const uint16_t selectAll   = br.readU16();
    const uint16_t affectMap   = br.readU16();
    const uint16_t map         = br.readU16();
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, false, devId)) { br.skip(br.remaining()); sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }

    if ((affectWhich & kMapNotifyMask) && affectMap) {
      st.mapNotifyMask &= (uint16_t)~affectMap;
      st.mapNotifyMask |= (uint16_t)(affectMap & map);
    }

    // Trailing {affect, details} pairs for each remaining bit in ascending
    // order, sized per event (xorg ProcXkbSelectEvents).
    auto apply = [&](auto& dst, uint32_t legal, uint8_t size, uint16_t bit, uint8_t ndx) -> bool {
      using T = std::remove_reference_t<decltype(dst)>;
      if (clear & bit)     { dst = 0; return true; }
      if (selectAll & bit) { dst = (T)~(T)0; return true; }
      if (br.remaining() < (size_t)size * 2u) { br.skip(br.remaining()); sendError(ctx, x11::error::BadLength, seq, 0, minor); return false; }
      uint32_t affect = 0, details = 0;
      if (size == 4)      { affect = br.readU32(); details = br.readU32(); }
      else if (size == 2) { affect = br.readU16(); details = br.readU16(); }
      else                { affect = br.readU8();  details = br.readU8(); if (br.remaining() >= 2) br.skip(2); }
      if (details & ~affect) { br.skip(br.remaining()); sendError(ctx, x11::error::BadMatch, seq, errCode2(ndx, details), minor); return false; }
      if (affect & ~legal)   { br.skip(br.remaining()); sendError(ctx, x11::error::BadValue, seq, errCode2(ndx, affect), minor); return false; }
      dst = (T)((dst & ~(T)affect) | (T)(affect & details));
      return true;
    };

    uint16_t maskLeft = (uint16_t)(affectWhich & ~kMapNotifyMask);
    for (uint8_t ndx = 0; maskLeft != 0 && ndx < 16; ndx++) {
      const uint16_t bit = (uint16_t)(1u << ndx);
      if (!(maskLeft & bit)) continue;
      maskLeft &= (uint16_t)~bit;
      bool ok = true;
      switch (ndx) {
        case 0:  ok = apply(st.newKeyboardNotifyMask, kAllNewKeyboardEventsMask, 2, bit, ndx); break;
        case 2:  ok = apply(st.stateNotifyMask,       kAllStateEventsMask,       2, bit, ndx); break;
        case 3:  ok = apply(st.ctrlsNotifyMask,       kAllControlEventsMask,     4, bit, ndx); break;
        case 4:  ok = apply(st.iStateNotifyMask,      kAllIndicatorEventsMask,   4, bit, ndx); break;
        case 5:  ok = apply(st.iMapNotifyMask,        kAllIndicatorEventsMask,   4, bit, ndx); break;
        case 6:  ok = apply(st.namesNotifyMask,       kAllNameEventsMask,        2, bit, ndx); break;
        case 7:  ok = apply(st.compatNotifyMask,      kAllCompatMapEventsMask,   1, bit, ndx); break;
        case 8:  ok = apply(st.bellNotifyMask,        kAllBellEventsMask,        1, bit, ndx); break;
        case 9:  ok = apply(st.actionMessageMask,     kAllActionMessagesMask,    1, bit, ndx); break;
        case 10: ok = apply(st.accessXNotifyMask,     kAllAccessXEventsMask,     2, bit, ndx); break;
        case 11: ok = apply(st.extDevNotifyMask,      kAllExtensionDeviceEventsMask, 2, bit, ndx); break;
        default:
          br.skip(br.remaining());
          sendError(ctx, x11::error::BadValue, seq, errCode2(33, bit), minor);
          return;
      }
      if (!ok) return;
    }
    br.skip(br.remaining());
    return;
  }

  // ---- 3: Bell (void) — validated like xorg, then a no-op (core Bell is too) ----
  case kBell: {
    if (!need(24)) return;
    const uint16_t deviceSpec = br.readU16();
    (void)br.readU16();                        // bellClass
    (void)br.readU16();                        // bellID
    const int8_t  percent    = br.readI8();
    const uint8_t forceSound = br.readU8();
    const uint8_t eventOnly  = br.readU8();
    br.skip(1);
    const int16_t pitch      = br.readI16();
    const int16_t duration   = br.readI16();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, false, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    if (forceSound && eventOnly) { sendError(ctx, x11::error::BadMatch, seq, errCode2(0x1, (forceSound << 8) | eventOnly), minor); return; }
    if (percent < -100 || percent > 100) { sendError(ctx, x11::error::BadValue, seq, errCode2(0x2, (uint8_t)percent), minor); return; }
    if (duration < -1) { sendError(ctx, x11::error::BadValue, seq, errCode2(0x3, (uint16_t)duration), minor); return; }
    if (pitch < -1)    { sendError(ctx, x11::error::BadValue, seq, errCode2(0x4, (uint16_t)pitch), minor); return; }
    return;
  }

  // ---- 4: GetState (reply) ----
  case kGetState: {
    if (!need(4)) return;
    const uint16_t deviceSpec = br.readU16();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, true, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    const uint8_t mods   = currentMods(ctx);
    const uint8_t locked = (uint8_t)(mods & kLockMask);
    const uint8_t base   = (uint8_t)(mods & ~kLockMask);
    const uint16_t btns  = currentButtonState(ctx);
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1]  = devId;
      rep[8]  = mods;      // mods (effective)
      rep[9]  = base;      // baseMods
      rep[10] = 0;         // latchedMods
      rep[11] = locked;    // lockedMods
      rep[12] = 0;         // group
      rep[13] = 0;         // lockedGroup
      wire::wr16_le(rep.data() + 14, 0);   // baseGroup
      wire::wr16_le(rep.data() + 16, 0);   // latchedGroup
      rep[18] = mods;      // compatState
      rep[19] = mods;      // grabMods
      rep[20] = mods;      // compatGrabMods
      rep[21] = mods;      // lookupMods
      rep[22] = mods;      // compatLookupMods
      wire::wr16_le(rep.data() + 24, btns); // ptrBtnState
    });
    return;
  }

  // ---- 5: LatchLockState (void) — locks/latches are not modelled ----
  case kLatchLockState: {
    if (!need(12)) return;
    const uint16_t deviceSpec = br.readU16();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, true, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    logOnce(minor, "LatchLockState");
    return;
  }

  // ---- 6: GetControls (reply, 92 bytes) ----
  case kGetControls: {
    if (!need(4)) return;
    const uint16_t deviceSpec = br.readU16();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, true, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    std::vector<uint8_t> rep;
    buildGetControlsReply(km, seq, devId, rep);
    sendVector(ctx, rep);
    return;
  }

  // ---- 8: GetMap (reply) ----
  case kGetMap: {
    if (!need(24)) return;
    GetMapRequest rq;
    rq.deviceSpec       = br.readU16();
    rq.full             = br.readU16();
    rq.partial          = br.readU16();
    rq.firstType        = br.readU8();
    rq.nTypes           = br.readU8();
    rq.firstKeySym      = br.readU8();
    rq.nKeySyms         = br.readU8();
    rq.firstKeyAct      = br.readU8();
    rq.nKeyActs         = br.readU8();
    rq.firstKeyBehavior = br.readU8();
    rq.nKeyBehaviors    = br.readU8();
    rq.virtualMods      = br.readU16();
    rq.firstKeyExplicit = br.readU8();
    rq.nKeyExplicit     = br.readU8();
    rq.firstModMapKey   = br.readU8();
    rq.nModMapKeys      = br.readU8();
    rq.firstVModMapKey  = br.readU8();
    rq.nVModMapKeys     = br.readU8();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(rq.deviceSpec, true, devId)) { sendBadKeyboard(ctx, seq, rq.deviceSpec, minor); return; }
    std::vector<uint8_t> rep;
    uint32_t errValue = 0;
    if (!buildGetMapReply(km, rq, seq, devId, rep, errValue)) {
      sendError(ctx, x11::error::BadValue, seq, errValue, minor);
      return;
    }
#ifndef NDEBUG
    TS_FPRINTF("[XKB] GetMap fd=%d full=0x%04X partial=0x%04X -> %zu bytes\n",
               ctx.transport().clientFd(), (unsigned)rq.full, (unsigned)rq.partial, rep.size());
#endif
    sendVector(ctx, rep);
    return;
  }

  // ---- 10: GetCompatMap (reply) ----
  case kGetCompatMap: {
    if (!need(8)) return;
    const uint16_t deviceSpec = br.readU16();
    const uint8_t  groups     = br.readU8();
    const uint8_t  getAllSI   = br.readU8();
    const uint16_t firstSI    = br.readU16();
    const uint16_t nSI        = br.readU16();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, true, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    std::vector<uint8_t> rep;
    uint32_t errValue = 0;
    if (!buildGetCompatMapReply(km, seq, devId, groups, getAllSI != 0, firstSI, nSI, rep, errValue)) {
      sendError(ctx, x11::error::BadValue, seq, errValue, minor);
      return;
    }
    sendVector(ctx, rep);
    return;
  }

  // ---- 12: GetIndicatorState (reply) ----
  case kGetIndicatorState: {
    if (!need(4)) return;
    const uint16_t deviceSpec = br.readU16();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, true, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    const uint32_t state = capsLockOn(ctx) ? 1u : 0u;   // bit 0 = Caps Lock
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = devId;
      wire::wr32_le(rep.data() + 8, state);
    });
    return;
  }

  // ---- 13: GetIndicatorMap (reply) ----
  case kGetIndicatorMap: {
    if (!need(8)) return;
    const uint16_t deviceSpec = br.readU16();
    br.skip(2);
    const uint32_t which = br.readU32();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, true, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    std::vector<uint8_t> rep;
    buildGetIndicatorMapReply(km, seq, devId, which, rep);
    sendVector(ctx, rep);
    return;
  }

  // ---- 15: GetNamedIndicator (reply) ----
  case kGetNamedIndicator: {
    if (!need(12)) return;
    const uint16_t deviceSpec = br.readU16();
    (void)br.readU16();      // ledClass
    (void)br.readU16();      // ledID
    br.skip(2);
    const uint32_t indicator = br.readU32();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, false, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    const int idx = km.indicatorIndexForAtom(indicator);
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = devId;
      wire::wr32_le(rep.data() + 8, indicator);
      rep[28] = 1;                       // supported
      if (idx < 0) { rep[12] = 0; return; }   // found = false
      const IndicatorMapDesc& m = km.indicatorMaps[(size_t)idx];
      rep[12] = 1;                                     // found
      rep[13] = (idx == 0 && capsLockOn(ctx)) ? 1 : 0; // on
      rep[14] = (km.physIndicators >> idx) & 1u;       // realIndicator
      rep[15] = (uint8_t)idx;                          // ndx
      rep[16] = m.flags;
      rep[17] = m.whichGroups;
      rep[18] = m.groups;
      rep[19] = m.whichMods;
      rep[20] = m.mods.mask;
      rep[21] = m.mods.realMods;
      wire::wr16_le(rep.data() + 22, m.mods.vmods);
      wire::wr32_le(rep.data() + 24, m.ctrls);
    });
    return;
  }

  // ---- 17: GetNames (reply) ----
  case kGetNames: {
    if (!need(8)) return;
    const uint16_t deviceSpec = br.readU16();
    br.skip(2);
    const uint32_t which = br.readU32();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, true, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    if (which & ~kAllNamesMask) { sendError(ctx, x11::error::BadValue, seq, errCode2(0x01, which), minor); return; }
    std::vector<uint8_t> rep;
    buildGetNamesReply(km, seq, devId, which, rep);
    sendVector(ctx, rep);
    return;
  }

  // ---- 19: GetGeometry (reply) — no geometry: found = false ----
  case kGetGeometry: {
    if (!need(8)) return;
    const uint16_t deviceSpec = br.readU16();
    br.skip(2);
    const uint32_t name = br.readU32();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, true, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = devId;
      wire::wr32_le(rep.data() + 8, name);
      rep[12] = 0;   // found
    });
    return;
  }

  // ---- 21: PerClientFlags (reply) ----
  case kPerClientFlags: {
    if (!need(24)) return;
    const uint16_t deviceSpec     = br.readU16();
    br.skip(2);
    const uint32_t change         = br.readU32();
    const uint32_t value          = br.readU32();
    const uint32_t ctrlsToChange  = br.readU32();
    const uint32_t autoCtrls      = br.readU32();
    const uint32_t autoCtrlValues = br.readU32();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, true, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    if (change & ~kPCF_AllFlagsMask) { sendError(ctx, x11::error::BadValue, seq, errCode2(0x01, change), minor); return; }
    if (value & ~change)             { sendError(ctx, x11::error::BadMatch, seq, errCode2(0x02, value), minor); return; }

    st.pcfFlags = (st.pcfFlags & ~change) | (value & change);
    if (change & kPCF_AutoResetControlsMask) {
      const bool want = (value & kPCF_AutoResetControlsMask) != 0;
      if (!want) {
        st.autoCtrls = st.autoCtrlValues = 0;
      } else {
        if (ctrlsToChange & ~kAllBooleanCtrlsMask) { sendError(ctx, x11::error::BadValue, seq, errCode2(0x03, ctrlsToChange), minor); return; }
        if (autoCtrls & ~ctrlsToChange)            { sendError(ctx, x11::error::BadMatch, seq, errCode2(0x04, autoCtrls), minor); return; }
        if (autoCtrlValues & ~autoCtrls)           { sendError(ctx, x11::error::BadMatch, seq, errCode2(0x05, autoCtrlValues), minor); return; }
        st.autoCtrls      = (st.autoCtrls & ~ctrlsToChange) | (autoCtrls & ctrlsToChange);
        st.autoCtrlValues = (st.autoCtrlValues & ~ctrlsToChange) | (autoCtrlValues & ctrlsToChange);
      }
    }
    // DetectableAutoRepeat needs no work: macOS delivers key repeats as
    // repeated keyDown with no keyUp in between, and X11WindowHost forwards
    // them as-is, so no synthetic KeyRelease is ever sent (any client).
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = devId;
      wire::wr32_le(rep.data() + 8,  kPCF_AllFlagsMask);            // supported
      wire::wr32_le(rep.data() + 12, st.pcfFlags & kPCF_AllFlagsMask); // value
      wire::wr32_le(rep.data() + 16, st.autoCtrls);
      wire::wr32_le(rep.data() + 20, st.autoCtrlValues);
    });
    return;
  }

  // ---- 22: ListComponents (reply) — no keymap database ----
  case kListComponents: {
    if (!need(4)) return;
    const uint16_t deviceSpec = br.readU16();
    br.skip(br.remaining());
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, false, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) { rep[1] = devId; });
    return;
  }

  // ---- 23: GetKbdByName (reply) — always the current keyboard; `load` is
  //          acknowledged but nothing is loaded (loaded = false) ----
  case kGetKbdByName: {
    if (!need(8)) return;
    const uint16_t deviceSpec = br.readU16();
    const uint16_t needMask   = br.readU16();
    const uint16_t wantMask   = br.readU16();
    const uint8_t  load       = br.readU8();
    br.skip(br.remaining());   // component name specs — ignored
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, true, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    if (wantMask & ~kGBN_AllComponentsMask) { sendError(ctx, x11::error::BadValue, seq, errCode2(0x01, wantMask), minor); return; }
    if (needMask & ~kGBN_AllComponentsMask) { sendError(ctx, x11::error::BadValue, seq, errCode2(0x02, needMask), minor); return; }

    uint16_t reported = load ? kGBN_AllComponentsMask : (uint16_t)(wantMask | needMask);
    reported &= (uint16_t)~kGBN_GeometryMask;   // no geometry

    std::vector<uint8_t> parts;
    if (reported & (kGBN_TypesMask | kGBN_ClientSymbolsMask | kGBN_ServerSymbolsMask)) {
      GetMapRequest rq;
      rq.deviceSpec = deviceSpec;
      if (reported & (kGBN_TypesMask | kGBN_ClientSymbolsMask)) rq.full |= kKeyTypesMask;
      if (reported & kGBN_ClientSymbolsMask) rq.full |= (uint16_t)(kKeySymsMask | kModifierMapMask);
      if (reported & kGBN_ServerSymbolsMask) rq.full |= kAllServerInfoMask;
      std::vector<uint8_t> m; uint32_t ev = 0;
      (void)buildGetMapReply(km, rq, seq, devId, m, ev);
      parts.insert(parts.end(), m.begin(), m.end());
    }
    if (reported & kGBN_CompatMapMask) {
      std::vector<uint8_t> c; uint32_t ev = 0;
      (void)buildGetCompatMapReply(km, seq, devId, 0x0f, true, 0, 0, c, ev);
      parts.insert(parts.end(), c.begin(), c.end());
    }
    if (reported & kGBN_IndicatorMapMask) {
      std::vector<uint8_t> i;
      buildGetIndicatorMapReply(km, seq, devId, 0xffffffffu, i);
      parts.insert(parts.end(), i.begin(), i.end());
    }
    if (reported & (kGBN_KeyNamesMask | kGBN_OtherNamesMask)) {
      uint32_t which = 0;
      if (reported & kGBN_OtherNamesMask) which |= kAllNamesMask & ~(kKeyNamesMask | kKeyAliasesMask);
      if (reported & kGBN_KeyNamesMask)   which |= (kKeyNamesMask | kKeyAliasesMask);
      std::vector<uint8_t> n;
      buildGetNamesReply(km, seq, devId, which, n);
      parts.insert(parts.end(), n.begin(), n.end());
    }

    std::vector<uint8_t> rep(32, 0);
    rep[0] = 1;
    rep[1] = devId;
    wire::wr16_le(rep.data() + 2, seq);
    wire::wr32_le(rep.data() + 4, (uint32_t)(parts.size() / 4u));
    rep[8]  = km.minKeyCode;
    rep[9]  = km.maxKeyCode;
    rep[10] = 0;   // loaded
    rep[11] = 0;   // newKeyboard
    wire::wr16_le(rep.data() + 12, reported);   // found
    wire::wr16_le(rep.data() + 14, reported);   // reported
    rep.insert(rep.end(), parts.begin(), parts.end());
    sendVector(ctx, rep);
    return;
  }

  // ---- 24: GetDeviceInfo (reply) ----
  case kGetDeviceInfo: {
    if (!need(12)) return;
    const uint16_t deviceSpec = br.readU16();
    const uint16_t wanted     = br.readU16();
    br.skip(br.remaining());   // allBtns, firstBtn, nBtns, pad, ledClass, ledID
    uint8_t devId = 0;
    if (!resolveDevice(deviceSpec, false, devId)) { sendBadKeyboard(ctx, seq, deviceSpec, minor); return; }
    if (wanted & ~kXI_AllFeaturesMask) { sendError(ctx, x11::error::BadValue, seq, errCode2(0x01, wanted), minor); return; }

    const bool kbd = isKeyboardId(devId);
    // Button actions and per-device indicator feedbacks are not modelled:
    // report them unsupported so the client parses no trailing sections.
    const uint16_t unsupported = (uint16_t)(wanted & kXI_AllDeviceFeaturesMask);
    const uint16_t present     = (uint16_t)(wanted & ~unsupported);
    const char* name = deviceName(devId);
    const uint16_t nameLen = (uint16_t)std::strlen(name);
    const size_t namePadded = ((size_t)nameLen + 2u + 3u) & ~(size_t)3u;
    const uint32_t devType = AtomTable::instance().intern(kbd ? "KEYBOARD" : "MOUSE", kbd ? 8 : 5, false);

    std::vector<uint8_t> rep(32 + namePadded, 0);
    rep[0] = 1;
    rep[1] = devId;
    wire::wr16_le(rep.data() + 2, seq);
    wire::wr32_le(rep.data() + 4, (uint32_t)(namePadded / 4u));
    wire::wr16_le(rep.data() + 8,  present);
    wire::wr16_le(rep.data() + 10, (uint16_t)(kXI_AllFeaturesMask & ~unsupported)); // supported
    wire::wr16_le(rep.data() + 12, unsupported);
    wire::wr16_le(rep.data() + 14, 0);           // nDeviceLedFBs
    rep[16] = 0; rep[17] = 0; rep[18] = 0; rep[19] = 0;   // firstBtnWanted.. nBtnsRtrn
    rep[20] = kbd ? 0 : 7;                       // totalBtns
    rep[21] = kbd ? 1 : 0;                       // hasOwnState
    wire::wr16_le(rep.data() + 22, kbd ? 0 : kXINone);   // dfltKbdFB
    wire::wr16_le(rep.data() + 24, kXINone);              // dfltLedFB
    wire::wr32_le(rep.data() + 28, devType);
    wire::wr16_le(rep.data() + 32, nameLen);
    std::memcpy(rep.data() + 34, name, nameLen);
    sendVector(ctx, rep);
    return;
  }

  // ---- 101: SetDebuggingFlags (reply) ----
  case kSetDebuggingFlags: {
    br.skip(br.remaining());
    (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>&) {});
    return;
  }

  // ---- void requests we accept but do not model ----
  case kSetControls:      br.skip(br.remaining()); logOnce(minor, "SetControls");      return;
  case kSetMap:           br.skip(br.remaining()); logOnce(minor, "SetMap");           return;
  case kSetCompatMap:     br.skip(br.remaining()); logOnce(minor, "SetCompatMap");     return;
  case kSetIndicatorMap:  br.skip(br.remaining()); logOnce(minor, "SetIndicatorMap");  return;
  case kSetNamedIndicator:br.skip(br.remaining()); logOnce(minor, "SetNamedIndicator");return;
  case kSetNames:         br.skip(br.remaining()); logOnce(minor, "SetNames");         return;
  case kSetGeometry:      br.skip(br.remaining()); logOnce(minor, "SetGeometry");      return;
  case kSetDeviceInfo:    br.skip(br.remaining()); logOnce(minor, "SetDeviceInfo");    return;

  default:
    break;
  }

  // Unknown minor — BadRequest, as xorg ProcXkbDispatch.
  {
    char buf[128];
    snprintf(buf, sizeof(buf), "[XKB] unhandled minor=%u seq=%u — sending BadRequest\n",
             (unsigned)minor, (unsigned)seq);
    x11_ui_push_log(1, buf);
  }
  br.skip(br.remaining());
  sendError(ctx, x11::error::BadRequest, seq, 0, minor);
}

} // namespace x11
