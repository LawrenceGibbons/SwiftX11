//
//  XkbKeymap.cpp
//  X11LowLevel
//
//  XKEYBOARD keymap model built from the core tables, and the wire builders
//  for the GetMap / GetNames / GetCompatMap / GetIndicatorMap / GetControls
//  replies.  See XkbKeymap.hpp for the design notes and the xorg references.
//

#include "Core/XkbKeymap.hpp"
#include "Core/CoreKeymap.hpp"
#include "Core/AtomTable.hpp"
#include "Core/KeySyms.hpp"

#include <cstring>

namespace x11::xkb {

// ---------------------------------------------------------------------------
// Little-endian byte writer
// ---------------------------------------------------------------------------
namespace {

struct Writer {
  std::vector<uint8_t>& v;
  void u8(uint8_t x)   { v.push_back(x); }
  void u16(uint16_t x) { v.push_back((uint8_t)(x & 0xff)); v.push_back((uint8_t)(x >> 8)); }
  void u32(uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)((x >> (8 * i)) & 0xff)); }
  void zeros(size_t n) { v.insert(v.end(), n, 0); }
  void pad4()          { while (v.size() % 4u) v.push_back(0); }
  void bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    v.insert(v.end(), b, b + n);
  }
};

inline void put16(std::vector<uint8_t>& v, size_t off, uint16_t x) {
  v[off] = (uint8_t)(x & 0xff); v[off + 1] = (uint8_t)(x >> 8);
}
inline void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
  for (int i = 0; i < 4; i++) v[off + i] = (uint8_t)((x >> (8 * i)) & 0xff);
}
inline uint32_t popcount32(uint32_t x) { return (uint32_t)__builtin_popcount(x); }

// Generic 32-byte reply header; body length is patched in by finish().
void beginReply(std::vector<uint8_t>& out, uint16_t seq, uint8_t deviceID, size_t headerBytes) {
  out.assign(headerBytes, 0);
  out[0] = 1;              // X_Reply
  out[1] = deviceID;
  put16(out, 2, seq);
}
// length = words after the first 32 bytes (extra header bytes count too).
void finishReply(std::vector<uint8_t>& out) {
  put32(out, 4, (uint32_t)((out.size() - 32u) / 4u));
}

// xorg _XkbErrCode2/3: high byte = sub-code, low 24 bits = offending value.
inline uint32_t errCode2(uint8_t a, uint32_t b) { return (uint32_t(a) << 24) | (b & 0xffffffu); }
inline uint32_t errCode3(uint8_t a, uint32_t b, uint32_t c) { return errCode2(a, ((b & 0xffff) << 8) | (c & 0xff)); }

// ---------------------------------------------------------------------------
// Keysym classification (xorg XkbUpdateMapFromCore / libX11 XConvertCase)
// ---------------------------------------------------------------------------
bool isAlphabeticPair(uint32_t lo, uint32_t hi) {
  if (lo >= 'a' && lo <= 'z') return hi == lo - 32u;
  if (lo >= 0xE0 && lo <= 0xFE && lo != 0xF7) return hi == lo - 32u;   // Latin-1
  return false;
}

bool isModifierKeysym(uint32_t s) {
  return (s >= XK_Shift_L && s <= 0xFFEE) ||   // Shift_L .. Hyper_R
         s == 0xFF7E ||                        // Mode_switch
         s == XK_Num_Lock;
}

// XKB key names for the macOS virtual keycodes we map (positional names as
// in xkeyboard-config's evdev/xfree86 keycodes files, so `xkbcomp` output
// reads like a stock keymap).  Unlisted used keycodes get "K<kc>".
struct VKName { uint8_t vk; const char* name; };
constexpr VKName kVKNames[] = {
  {0,"AC01"},{1,"AC02"},{2,"AC03"},{3,"AC04"},{4,"AC06"},{5,"AC05"},
  {6,"AB01"},{7,"AB02"},{8,"AB03"},{9,"AB04"},{10,"LSGT"},{11,"AB05"},
  {12,"AD01"},{13,"AD02"},{14,"AD03"},{15,"AD04"},{16,"AD06"},{17,"AD05"},
  {18,"AE01"},{19,"AE02"},{20,"AE03"},{21,"AE04"},{22,"AE06"},{23,"AE05"},
  {24,"AE12"},{25,"AE09"},{26,"AE07"},{27,"AE11"},{28,"AE08"},{29,"AE10"},
  {30,"AD12"},{31,"AD09"},{32,"AD07"},{33,"AD11"},{34,"AD08"},{35,"AD10"},
  {36,"RTRN"},{37,"AC09"},{38,"AC07"},{39,"AC11"},{40,"AC08"},{41,"AC10"},
  {42,"BKSL"},{43,"AB08"},{44,"AB10"},{45,"AB06"},{46,"AB07"},{47,"AB09"},
  {48,"TAB"},{49,"SPCE"},{50,"TLDE"},{51,"BKSP"},{53,"ESC"},
  {54,"RWIN"},{55,"LWIN"},{56,"LFSH"},{57,"CAPS"},{58,"LALT"},{59,"LCTL"},
  {60,"RTSH"},{61,"RALT"},{62,"RCTL"},{63,"FN"},
  {64,"FK17"},{65,"KPDL"},{67,"KPMU"},{69,"KPAD"},{71,"NMLK"},{75,"KPDV"},
  {76,"KPEN"},{78,"KPSU"},{79,"FK18"},{80,"FK19"},{81,"KPEQ"},
  {82,"KP0"},{83,"KP1"},{84,"KP2"},{85,"KP3"},{86,"KP4"},{87,"KP5"},
  {88,"KP6"},{89,"KP7"},{90,"FK20"},{91,"KP8"},{92,"KP9"},
  {96,"FK05"},{97,"FK06"},{98,"FK07"},{99,"FK03"},{100,"FK08"},{101,"FK09"},
  {103,"FK11"},{105,"FK13"},{106,"FK16"},{107,"FK14"},{109,"FK10"},{111,"FK12"},
  {113,"FK15"},{114,"INS"},{115,"HOME"},{116,"PGUP"},{117,"END"},{118,"FK04"},
  {119,"DELE"},{120,"FK02"},{121,"PGDN"},{122,"FK01"},
  {123,"LEFT"},{124,"RGHT"},{125,"DOWN"},{126,"UP"},
};

void setKeyName(KeyDesc& k, const char* s) {
  k.name.fill(0);
  for (size_t i = 0; i < 4 && s[i]; i++) k.name[i] = s[i];
}

// ---------------------------------------------------------------------------
// Build from the core tables
// ---------------------------------------------------------------------------
Keymap buildFromCore() {
  Keymap km;
  km.minKeyCode = kCoreMinKeyCode;
  km.maxKeyCode = kCoreMaxKeyCode;

  AtomTable& at = AtomTable::instance();
  auto atom = [&](const char* s) -> uint32_t { return at.intern(s, std::strlen(s), false); };

  // --- Canonical key types (XkbInitCanonicalKeyTypes; types/basic + numpad) ---
  {
    KeyType t;
    t.mods = {0, 0, 0};
    t.numLevels = 1;
    t.nameAtom = atom("ONE_LEVEL");
    t.levelNameAtoms = { atom("Any") };
    km.types.push_back(t);
  }
  {
    KeyType t;
    t.mods = {kShiftMask, kShiftMask, 0};
    t.numLevels = 2;
    t.map = { KeyTypeEntry{true, {kShiftMask, kShiftMask, 0}, 1} };
    t.nameAtom = atom("TWO_LEVEL");
    t.levelNameAtoms = { atom("Base"), atom("Shift") };
    km.types.push_back(t);
  }
  {
    KeyType t;
    t.mods = {(uint8_t)(kShiftMask | kLockMask), (uint8_t)(kShiftMask | kLockMask), 0};
    t.numLevels = 2;
    // Shift+Lock matches no entry → level 0, i.e. Shift undoes Caps Lock,
    // exactly as types/basic ALPHABETIC.
    t.map = { KeyTypeEntry{true, {kShiftMask, kShiftMask, 0}, 1},
              KeyTypeEntry{true, {kLockMask,  kLockMask,  0}, 1} };
    t.nameAtom = atom("ALPHABETIC");
    t.levelNameAtoms = { atom("Base"), atom("Caps") };
    km.types.push_back(t);
  }
  {
    const uint16_t numLockV = (uint16_t)(1u << kVModNumLock);
    KeyType t;
    t.mods = {(uint8_t)(kShiftMask | kMod2Mask), kShiftMask, numLockV};
    t.numLevels = 2;
    t.map = { KeyTypeEntry{true, {kShiftMask, kShiftMask, 0}, 1},
              KeyTypeEntry{true, {kMod2Mask, 0, numLockV}, 1} };
    t.nameAtom = atom("KEYPAD");
    t.levelNameAtoms = { atom("Base"), atom("Number") };
    km.types.push_back(t);
  }

  // --- Virtual modifiers (xkeyboard-config names; bindings as on a stock
  //     xorg pc105/us keymap: Alt/Meta→Mod1, Super/Hyper→Mod4, NumLock→Mod2,
  //     LevelThree/AltGr→Mod5, L/RControl→Control) ---
  {
    struct V { uint8_t idx; const char* name; uint8_t real; };
    const V vm[] = {
      {kVModNumLock,    "NumLock",    kMod2Mask},
      {kVModAlt,        "Alt",        kMod1Mask},
      {kVModLevelThree, "LevelThree", kMod5Mask},
      {kVModLAlt,       "LAlt",       kMod1Mask},
      {kVModRAlt,       "RAlt",       kMod1Mask},
      {kVModRControl,   "RControl",   kControlMask},
      {kVModLControl,   "LControl",   kControlMask},
      {kVModScrollLock, "ScrollLock", 0},
      {kVModLevelFive,  "LevelFive",  0},
      {kVModAltGr,      "AltGr",      kMod5Mask},
      {kVModMeta,       "Meta",       kMod1Mask},
      {kVModSuper,      "Super",      kMod4Mask},
      {kVModHyper,      "Hyper",      kMod4Mask},
    };
    for (const V& v : vm) {
      km.vmodDefined |= (uint16_t)(1u << v.idx);
      km.vmodRealMods[v.idx] = v.real;
      km.vmodNameAtoms[v.idx] = atom(v.name);
    }
  }

  // --- Keys from the core keysym table ---
  const KeySyms4* core = coreKeyboardMap();
  for (unsigned kc = km.minKeyCode; kc <= km.maxKeyCode; kc++) {
    KeyDesc& k = km.keys[kc];
    const uint32_t s0 = core[kc].syms[0];
    const uint32_t s1 = core[kc].syms[1];

    // Name (positional where known, else K<keycode>)
    {
      const uint8_t vk = (uint8_t)(kc - 8u);
      const char* nm = nullptr;
      for (const VKName& e : kVKNames) if (e.vk == vk) { nm = e.name; break; }
      char buf[8];
      if (!nm) { std::snprintf(buf, sizeof(buf), "K%u", kc); nm = buf; }
      if (s0 != XK_NoSymbol || nm[0] != 'K') setKeyName(k, nm);
    }

    if (s0 == XK_NoSymbol) continue;   // unused keycode: 0 groups, no syms

    k.numGroups = 1;
    if (isAlphabeticPair(s0, s1)) {
      k.ktIndex[0] = kTypeAlphabetic; k.width = 2; k.syms = { s0, s1 };
    } else if (s1 == XK_NoSymbol || s1 == s0) {
      k.ktIndex[0] = kTypeOneLevel;   k.width = 1; k.syms = { s0 };
    } else {
      k.ktIndex[0] = kTypeTwoLevel;   k.width = 2; k.syms = { s0, s1 };
    }
    k.repeats = !isModifierKeysym(s0);
  }

  // --- Modifier map from the core rows (Shift, Lock, Control, Mod1..Mod5) ---
  {
    uint8_t perMod = 0;
    const uint8_t* rows = coreModifierMap(perMod);
    for (unsigned row = 0; row < 8; row++) {
      for (unsigned i = 0; i < perMod; i++) {
        const uint8_t kc = rows[row * perMod + i];
        if (kc && km.inRange(kc)) km.keys[kc].modmap |= (uint8_t)(1u << row);
      }
    }
  }

  // --- Virtual modifier map (what compat interprets would derive on xorg) ---
  {
    auto vm = [&](uint8_t vk, uint16_t bits) {
      KeyDesc& k = km.keys[macToX11Keycode(vk)];
      if (k.used()) k.vmodmap |= bits;
    };
    const uint16_t alt = (uint16_t)(1u << kVModAlt), meta = (uint16_t)(1u << kVModMeta);
    vm(58, (uint16_t)(alt | (1u << kVModLAlt) | meta));   // Option_L
    vm(61, (uint16_t)(alt | (1u << kVModRAlt) | meta));   // Option_R
    vm(55, (uint16_t)(1u << kVModSuper));                 // Command_L
    vm(54, (uint16_t)(1u << kVModSuper));                 // Command_R
    vm(63, meta);                                         // Fn (Meta_L)
    vm(59, (uint16_t)(1u << kVModLControl));              // Control_L
    vm(62, (uint16_t)(1u << kVModRControl));              // Control_R
  }

  // --- Indicators (Caps Lock / Num Lock / Scroll Lock, as compat/ledcaps etc.) ---
  {
    km.physIndicators = 0x7;
    struct I { uint8_t idx; const char* name; ModsDesc mods; };
    const I leds[] = {
      {0, "Caps Lock",   {kLockMask, kLockMask, 0}},
      {1, "Num Lock",    {kMod2Mask, 0, (uint16_t)(1u << kVModNumLock)}},
      {2, "Scroll Lock", {0, 0, (uint16_t)(1u << kVModScrollLock)}},
    };
    for (const I& l : leds) {
      km.indicatorNameAtoms[l.idx] = atom(l.name);
      IndicatorMapDesc& m = km.indicatorMaps[l.idx];
      m.flags = kIM_NoExplicit;
      m.whichMods = kIM_UseLocked;
      m.mods = l.mods;
    }
  }

  // --- Component / group names ---
  km.keycodesNameAtom = atom("swiftx11");
  km.symbolsNameAtom  = atom("pc+us");
  km.typesNameAtom    = atom("complete");
  km.compatNameAtom   = atom("complete");
  km.groupNameAtoms[0] = atom("English (US)");

  // --- Controls: per-key repeat for every non-modifier key ---
  for (unsigned kc = km.minKeyCode; kc <= km.maxKeyCode; kc++) {
    const KeyDesc& k = km.keys[kc];
    if (k.used() && k.repeats) km.controls.perKeyRepeat[kc >> 3] |= (uint8_t)(1u << (kc & 7u));
  }

  return km;
}

} // namespace

// ---------------------------------------------------------------------------
// Keymap
// ---------------------------------------------------------------------------
const Keymap& Keymap::current() {
  static const Keymap km = buildFromCore();
  return km;
}

int Keymap::indicatorIndexForAtom(uint32_t a) const {
  if (a == 0) return -1;
  for (unsigned i = 0; i < kNumIndicators; i++)
    if (indicatorNameAtoms[i] == a) return (int)i;
  return -1;
}

// ---------------------------------------------------------------------------
// GetMap (xkb/xkb.c ProcXkbGetMap + XkbSendMap)
// ---------------------------------------------------------------------------
bool buildGetMapReply(const Keymap& km, const GetMapRequest& rq, uint16_t seq,
                      uint8_t deviceID, std::vector<uint8_t>& out, uint32_t& errValue)
{
  // CHK_MASK_LEGAL / CHK_MASK_OVERLAP
  if (rq.full & ~kAllMapComponentsMask)    { errValue = errCode2(0x01, rq.full);    return false; }
  if (rq.partial & ~kAllMapComponentsMask) { errValue = errCode2(0x02, rq.partial); return false; }
  if (rq.full & rq.partial)                { errValue = errCode3(0x03, rq.full, rq.partial); return false; }

  uint16_t present = (uint16_t)(rq.full | rq.partial);
  const uint8_t  minKC = km.minKeyCode, maxKC = km.maxKeyCode;
  const uint16_t nAll  = km.numKeys();

  // Resolve a per-key range: `full` = whole keyboard, `partial` = client range
  // (CHK_KEY_RANGE: BadValue if it leaves [min, max]).
  auto keyRange = [&](uint16_t bit, uint8_t rqFirst, uint8_t rqN, uint8_t errSub,
                      uint8_t& first, uint8_t& n) -> bool {
    first = 0; n = 0;
    if (rq.full & bit) { first = minKC; n = (uint8_t)nAll; return true; }
    if (rq.partial & bit) {
      if (rqFirst < minKC || (int)rqFirst + (int)rqN - 1 > (int)maxKC) {
        errValue = errCode3(errSub, rqFirst, rqN);
        return false;
      }
      first = rqFirst; n = rqN;
    }
    return true;
  };

  // --- key types ---
  uint8_t firstType = 0, nTypes = 0;
  const uint8_t totalTypes = (uint8_t)km.types.size();
  if (rq.full & kKeyTypesMask) { firstType = 0; nTypes = totalTypes; }
  else if (rq.partial & kKeyTypesMask) {
    if ((int)rq.firstType + (int)rq.nTypes > (int)totalTypes) {
      errValue = errCode3(0x04, rq.firstType, rq.nTypes); return false;
    }
    firstType = rq.firstType; nTypes = rq.nTypes;
  }
  if ((present & kKeyTypesMask) && nTypes < 1) { present &= ~kKeyTypesMask; firstType = nTypes = 0; }

  uint8_t firstKeySym = 0, nKeySyms = 0;
  if (!keyRange(kKeySymsMask, rq.firstKeySym, rq.nKeySyms, 0x05, firstKeySym, nKeySyms)) return false;
  if ((present & kKeySymsMask) && nKeySyms < 1) { present &= ~kKeySymsMask; firstKeySym = nKeySyms = 0; }

  uint8_t firstKeyAct = 0, nKeyActs = 0;
  if (!keyRange(kKeyActionsMask, rq.firstKeyAct, rq.nKeyActs, 0x06, firstKeyAct, nKeyActs)) return false;
  if ((present & kKeyActionsMask) && nKeyActs < 1) { present &= ~kKeyActionsMask; firstKeyAct = nKeyActs = 0; }

  uint8_t firstKeyBehavior = 0, nKeyBehaviors = 0;
  if (!keyRange(kKeyBehaviorsMask, rq.firstKeyBehavior, rq.nKeyBehaviors, 0x07, firstKeyBehavior, nKeyBehaviors)) return false;
  if ((present & kKeyBehaviorsMask) && nKeyBehaviors < 1) { present &= ~kKeyBehaviorsMask; firstKeyBehavior = nKeyBehaviors = 0; }

  uint16_t virtualMods = 0;
  if (rq.full & kVirtualModsMask) virtualMods = 0xFFFF;         // xorg: ~0 for full
  else if (rq.partial & kVirtualModsMask) virtualMods = rq.virtualMods;
  if ((present & kVirtualModsMask) && virtualMods == 0) present &= ~kVirtualModsMask;

  uint8_t firstKeyExplicit = 0, nKeyExplicit = 0;
  if (!keyRange(kExplicitComponentsMask, rq.firstKeyExplicit, rq.nKeyExplicit, 0x08, firstKeyExplicit, nKeyExplicit)) return false;
  if ((present & kExplicitComponentsMask) && nKeyExplicit < 1) { present &= ~kExplicitComponentsMask; firstKeyExplicit = nKeyExplicit = 0; }

  uint8_t firstModMapKey = 0, nModMapKeys = 0;
  if (!keyRange(kModifierMapMask, rq.firstModMapKey, rq.nModMapKeys, 0x09, firstModMapKey, nModMapKeys)) return false;
  if ((present & kModifierMapMask) && nModMapKeys < 1) { present &= ~kModifierMapMask; firstModMapKey = nModMapKeys = 0; }

  uint8_t firstVModMapKey = 0, nVModMapKeys = 0;
  if (!keyRange(kVirtualModMapMask, rq.firstVModMapKey, rq.nVModMapKeys, 0x0a, firstVModMapKey, nVModMapKeys)) return false;
  if ((present & kVirtualModMapMask) && nVModMapKeys < 1) { present &= ~kVirtualModMapMask; firstVModMapKey = nVModMapKeys = 0; }

  // --- body, in XkbSendMap order ---
  std::vector<uint8_t> body;
  Writer w{body};

  // key types: xkbKeyTypeWireDesc + entries + preserve
  if (present & kKeyTypesMask) {
    for (unsigned i = firstType; i < (unsigned)firstType + nTypes; i++) {
      const KeyType& t = km.types[i];
      const bool hasPreserve = !t.preserve.empty();
      w.u8(t.mods.mask); w.u8(t.mods.realMods); w.u16(t.mods.vmods);
      w.u8(t.numLevels); w.u8((uint8_t)t.map.size()); w.u8(hasPreserve ? 1 : 0); w.u8(0);
      for (const KeyTypeEntry& e : t.map) {
        w.u8(e.active ? 1 : 0); w.u8(e.mods.mask); w.u8(e.level); w.u8(e.mods.realMods);
        w.u16(e.mods.vmods); w.u16(0);
      }
      if (hasPreserve) {
        for (size_t j = 0; j < t.map.size(); j++) {
          const ModsDesc p = (j < t.preserve.size()) ? t.preserve[j] : ModsDesc{};
          w.u8(p.mask); w.u8(p.realMods); w.u16(p.vmods);
        }
      }
    }
  }

  // key syms: xkbSymMapWireDesc + keysyms
  uint16_t totalSyms = 0;
  if (present & kKeySymsMask) {
    for (unsigned kc = firstKeySym; kc < (unsigned)firstKeySym + nKeySyms; kc++) {
      const KeyDesc& k = km.keys[kc];
      for (int g = 0; g < 4; g++) w.u8(k.ktIndex[g]);
      w.u8(k.groupInfo()); w.u8(k.width); w.u16((uint16_t)k.syms.size());
      for (uint32_t s : k.syms) w.u32(s);
      totalSyms = (uint16_t)(totalSyms + k.syms.size());
    }
  }

  // key actions: one count byte per key (all 0 — no actions), padded; no acts
  uint16_t totalActs = 0;
  if (present & kKeyActionsMask) {
    w.zeros(nKeyActs);
    w.pad4();
  }

  // key behaviors: none (totalKeyBehaviors = 0)
  const uint8_t totalKeyBehaviors = 0;

  // virtual mods: one real-mod byte per set bit, padded
  if (present & kVirtualModsMask) {
    for (unsigned i = 0; i < kNumVirtualMods; i++) {
      if (virtualMods & (1u << i))
        w.u8((km.vmodDefined & (1u << i)) ? km.vmodRealMods[i] : 0);
    }
    w.pad4();
  }

  // explicit components: (key, explicit) pairs for non-zero keys, padded
  uint8_t totalKeyExplicit = 0;
  if (present & kExplicitComponentsMask) {
    for (unsigned kc = firstKeyExplicit; kc < (unsigned)firstKeyExplicit + nKeyExplicit; kc++) {
      const KeyDesc& k = km.keys[kc];
      if (k.explicitMask) { w.u8((uint8_t)kc); w.u8(k.explicitMask); totalKeyExplicit++; }
    }
    w.pad4();
  }

  // modifier map: (key, mods) pairs for non-zero keys, padded
  uint8_t totalModMapKeys = 0;
  if (present & kModifierMapMask) {
    for (unsigned kc = firstModMapKey; kc < (unsigned)firstModMapKey + nModMapKeys; kc++) {
      const KeyDesc& k = km.keys[kc];
      if (k.modmap) { w.u8((uint8_t)kc); w.u8(k.modmap); totalModMapKeys++; }
    }
    w.pad4();
  }

  // virtual modifier map: xkbVModMapWireDesc {key, pad, vmods} for non-zero keys
  uint8_t totalVModMapKeys = 0;
  if (present & kVirtualModMapMask) {
    for (unsigned kc = firstVModMapKey; kc < (unsigned)firstVModMapKey + nVModMapKeys; kc++) {
      const KeyDesc& k = km.keys[kc];
      if (k.vmodmap) { w.u8((uint8_t)kc); w.u8(0); w.u16(k.vmodmap); totalVModMapKeys++; }
    }
  }

  // --- 40-byte header (xkbGetMapReply) ---
  beginReply(out, seq, deviceID, 40);
  // [8..9] pad1
  out[10] = minKC;
  out[11] = maxKC;
  put16(out, 12, present);
  out[14] = firstType;
  out[15] = nTypes;
  out[16] = totalTypes;
  out[17] = firstKeySym;
  put16(out, 18, totalSyms);
  out[20] = nKeySyms;
  out[21] = firstKeyAct;
  put16(out, 22, totalActs);
  out[24] = nKeyActs;
  out[25] = firstKeyBehavior;
  out[26] = nKeyBehaviors;
  out[27] = totalKeyBehaviors;
  out[28] = firstKeyExplicit;
  out[29] = nKeyExplicit;
  out[30] = totalKeyExplicit;
  out[31] = firstModMapKey;
  out[32] = nModMapKeys;
  out[33] = totalModMapKeys;
  out[34] = firstVModMapKey;
  out[35] = nVModMapKeys;
  out[36] = totalVModMapKeys;
  out[37] = 0;
  put16(out, 38, virtualMods);

  out.insert(out.end(), body.begin(), body.end());
  finishReply(out);
  return true;
}

// ---------------------------------------------------------------------------
// GetCompatMap (ProcXkbGetCompatMap + XkbSendCompatMap) — no sym interprets
// ---------------------------------------------------------------------------
bool buildGetCompatMapReply(const Keymap& /*km*/, uint16_t seq, uint8_t deviceID,
                            uint8_t groups, bool getAllSI, uint16_t firstSI, uint16_t nSI,
                            std::vector<uint8_t>& out, uint32_t& errValue)
{
  const uint16_t numSI = 0;
  if (getAllSI) { firstSI = 0; nSI = numSI; }
  else if (nSI > 0 && (unsigned)firstSI + nSI - 1 >= numSI) {
    errValue = errCode2(0x05, numSI);
    return false;
  }
  const uint8_t grp = (uint8_t)(groups & 0x0f);

  beginReply(out, seq, deviceID, 32);
  out[8] = grp;
  put16(out, 10, firstSI);
  put16(out, 12, nSI);
  put16(out, 14, numSI);
  // body: nSI × xkbSymInterpretWireDesc (none) + one xkbModsWireDesc per group bit
  Writer w{out};
  for (unsigned i = 0; i < kNumKbdGroups; i++) {
    if (grp & (1u << i)) { w.u8(0); w.u8(0); w.u16(0); }
  }
  finishReply(out);
  return true;
}

// ---------------------------------------------------------------------------
// GetIndicatorMap (ProcXkbGetIndicatorMap + XkbSendIndicatorMap)
// ---------------------------------------------------------------------------
void buildGetIndicatorMapReply(const Keymap& km, uint16_t seq, uint8_t deviceID,
                               uint32_t which, std::vector<uint8_t>& out)
{
  beginReply(out, seq, deviceID, 32);
  put32(out, 8, which);
  put32(out, 12, km.physIndicators);
  out[16] = (uint8_t)popcount32(which);
  Writer w{out};
  for (unsigned i = 0; i < kNumIndicators; i++) {
    if (!(which & (1u << i))) continue;
    const IndicatorMapDesc& m = km.indicatorMaps[i];
    w.u8(m.flags); w.u8(m.whichGroups); w.u8(m.groups); w.u8(m.whichMods);
    w.u8(m.mods.mask); w.u8(m.mods.realMods); w.u16(m.mods.vmods); w.u32(m.ctrls);
  }
  finishReply(out);
}

// ---------------------------------------------------------------------------
// GetNames (XkbComputeGetNamesReplySize + XkbSendNames)
// ---------------------------------------------------------------------------
void buildGetNamesReply(const Keymap& km, uint16_t seq, uint8_t deviceID,
                        uint32_t whichIn, std::vector<uint8_t>& out)
{
  uint32_t which = whichIn & kAllNamesMask;
  const uint8_t nTypes = (uint8_t)km.types.size();
  const uint8_t firstKey = km.minKeyCode;
  const uint8_t nKeys = (uint8_t)km.numKeys();

  // Masks of the entries that actually carry a name; xorg drops the section
  // bit when none does.
  uint32_t indicators = 0;
  for (unsigned i = 0; i < kNumIndicators; i++) if (km.indicatorNameAtoms[i]) indicators |= (1u << i);
  uint16_t vmods = 0;
  for (unsigned i = 0; i < kNumVirtualMods; i++) if (km.vmodNameAtoms[i]) vmods |= (uint16_t)(1u << i);
  uint8_t groupNames = 0;
  for (unsigned i = 0; i < kNumKbdGroups; i++) if (km.groupNameAtoms[i]) groupNames |= (uint8_t)(1u << i);

  if ((which & kIndicatorNamesMask) && indicators == 0) which &= ~kIndicatorNamesMask;
  if ((which & kVirtualModNamesMask) && vmods == 0)     which &= ~kVirtualModNamesMask;
  if ((which & kGroupNamesMask) && groupNames == 0)      which &= ~kGroupNamesMask;
  which &= ~(kKeyAliasesMask | kRGNamesMask);            // none defined
  if (!(which & kIndicatorNamesMask)) indicators = 0;
  if (!(which & kVirtualModNamesMask)) vmods = 0;
  if (!(which & kGroupNamesMask)) groupNames = 0;

  std::vector<uint8_t> body;
  Writer w{body};
  if (which & kKeycodesNameMask)    w.u32(km.keycodesNameAtom);
  if (which & kGeometryNameMask)    w.u32(km.geometryNameAtom);
  if (which & kSymbolsNameMask)     w.u32(km.symbolsNameAtom);
  if (which & kPhysSymbolsNameMask) w.u32(km.physSymbolsNameAtom);
  if (which & kTypesNameMask)       w.u32(km.typesNameAtom);
  if (which & kCompatNameMask)      w.u32(km.compatNameAtom);
  if (which & kKeyTypeNamesMask) {
    for (const KeyType& t : km.types) w.u32(t.nameAtom);
  }
  uint16_t nKTLevels = 0;
  if (which & kKTLevelNamesMask) {
    // nTypes level counts (padded), then that many atoms per type.  Both
    // libX11 (XKBNames.c:133) and libxkbcommon 1.0.3 require the count to
    // equal the type's numLevels.
    for (const KeyType& t : km.types) w.u8(t.numLevels);
    w.pad4();
    for (const KeyType& t : km.types) {
      for (unsigned l = 0; l < t.numLevels; l++)
        w.u32(l < t.levelNameAtoms.size() ? t.levelNameAtoms[l] : 0);
      nKTLevels = (uint16_t)(nKTLevels + t.numLevels);
    }
  }
  if (which & kIndicatorNamesMask) {
    for (unsigned i = 0; i < kNumIndicators; i++) if (indicators & (1u << i)) w.u32(km.indicatorNameAtoms[i]);
  }
  if (which & kVirtualModNamesMask) {
    for (unsigned i = 0; i < kNumVirtualMods; i++) if (vmods & (1u << i)) w.u32(km.vmodNameAtoms[i]);
  }
  if (which & kGroupNamesMask) {
    for (unsigned i = 0; i < kNumKbdGroups; i++) if (groupNames & (1u << i)) w.u32(km.groupNameAtoms[i]);
  }
  if (which & kKeyNamesMask) {
    for (unsigned kc = firstKey; kc < (unsigned)firstKey + nKeys; kc++) w.bytes(km.keys[kc].name.data(), 4);
  }
  // key aliases, radio group names: none

  beginReply(out, seq, deviceID, 32);
  put32(out, 8, which);
  out[12] = km.minKeyCode;
  out[13] = km.maxKeyCode;
  out[14] = nTypes;
  out[15] = groupNames;
  put16(out, 16, vmods);
  out[18] = (which & kKeyNamesMask) ? firstKey : 0;
  out[19] = (which & kKeyNamesMask) ? nKeys : 0;
  put32(out, 20, indicators);
  out[24] = 0;   // nRadioGroups
  out[25] = 0;   // nKeyAliases
  put16(out, 26, nKTLevels);
  out.insert(out.end(), body.begin(), body.end());
  finishReply(out);
}

// ---------------------------------------------------------------------------
// GetControls (ProcXkbGetControls) — 92 bytes, length 15
// ---------------------------------------------------------------------------
void buildGetControlsReply(const Keymap& km, uint16_t seq, uint8_t deviceID,
                           std::vector<uint8_t>& out)
{
  const Controls& c = km.controls;
  beginReply(out, seq, deviceID, 92);
  out[8]  = c.mkDfltBtn;
  out[9]  = c.numGroups;
  out[10] = c.groupsWrap;
  out[11] = c.internal.mask;
  out[12] = c.ignoreLock.mask;
  out[13] = c.internal.realMods;
  out[14] = c.ignoreLock.realMods;
  out[15] = 0;
  put16(out, 16, c.internal.vmods);
  put16(out, 18, c.ignoreLock.vmods);
  put16(out, 20, c.repeatDelay);
  put16(out, 22, c.repeatInterval);
  put16(out, 24, c.slowKeysDelay);
  put16(out, 26, c.debounceDelay);
  put16(out, 28, c.mkDelay);
  put16(out, 30, c.mkInterval);
  put16(out, 32, c.mkTimeToMax);
  put16(out, 34, c.mkMaxSpeed);
  put16(out, 36, (uint16_t)c.mkCurve);
  put16(out, 38, c.axOptions);
  put16(out, 40, c.axTimeout);
  put16(out, 42, c.axtOptsMask);
  put16(out, 44, c.axtOptsValues);
  put16(out, 46, 0);
  put32(out, 48, c.axtCtrlsMask);
  put32(out, 52, c.axtCtrlsValues);
  put32(out, 56, c.enabledCtrls);
  std::memcpy(out.data() + 60, c.perKeyRepeat.data(), 32);
  finishReply(out);   // (92 - 32) / 4 = 15
}

} // namespace x11::xkb
