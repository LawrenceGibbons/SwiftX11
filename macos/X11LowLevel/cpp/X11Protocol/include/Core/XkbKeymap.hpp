//
//  XkbKeymap.hpp
//  X11LowLevel
//
//  XKEYBOARD keymap model and wire builders (Phase F / M23, v1.20.0.20).
//
//  The model is derived once from the core tables in Core/CoreKeymap.hpp
//  (keycode → keysym table, modifier map) so the XKB and core views of the
//  keyboard never disagree.  It follows xorg's XkbUpdateMapFromCore rules for
//  a single-group keyboard: four canonical key types (ONE_LEVEL, TWO_LEVEL,
//  ALPHABETIC, KEYPAD — libX11's XkbAllocClientMap refuses 1..3 types), one
//  group per key, width = the type's level count, nSyms = width, modmap from
//  the core modifier rows, and the xkeyboard-config virtual-modifier names so
//  GDK's <Super>/<Meta>/<Hyper> accelerators resolve.
//
//  The wire builders produce complete replies (32-byte generic header with
//  the length field filled, plus body) as byte vectors, so GetKbdByName can
//  concatenate them exactly the way xorg's ProcXkbGetKbdByName does.  Layouts
//  follow xorgproto XKBproto.h and xorg xkb/xkb.c (XkbSendMap, XkbSendNames,
//  XkbSendCompatMap, XkbSendIndicatorMap, ProcXkbGetControls); the per-client
//  parser constraints of libX11 (XKBGetMap.c), GDK 3.24 (gdkkeys-x11.c),
//  libxkbcommon 1.0.3 (src/x11/keymap.c) and Chromium 124 (XKBBind.c port)
//  are recorded in docs/XI2_XORG_COMPARISON.md §Phase F.
//
//  This header has no dependency on the protocol context so the builders can
//  be exercised from a standalone harness.
//

#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace x11::xkb {

// ---------------------------------------------------------------------------
// Protocol constants (xorgproto XKB.h)
// ---------------------------------------------------------------------------
constexpr uint16_t kUseCoreKbd  = 0x0100;
constexpr uint16_t kUseCorePtr  = 0x0200;
constexpr uint16_t kDfltXIClass = 0x0300;
constexpr uint16_t kDfltXIId    = 0x0400;
constexpr uint16_t kXINone      = 0xff00;

// XI2 device ids (Core/XI2EventMask.hpp): the core keyboard is master 3.
constexpr uint8_t kCorePointerId  = 2;
constexpr uint8_t kCoreKeyboardId = 3;

constexpr uint8_t kNumVirtualMods = 16;
constexpr uint8_t kNumIndicators  = 32;
constexpr uint8_t kNumKbdGroups   = 4;

// Map components (XkbGetMap full/partial, MapNotify changed)
constexpr uint16_t kKeyTypesMask           = 1u << 0;
constexpr uint16_t kKeySymsMask            = 1u << 1;
constexpr uint16_t kModifierMapMask        = 1u << 2;
constexpr uint16_t kExplicitComponentsMask = 1u << 3;
constexpr uint16_t kKeyActionsMask         = 1u << 4;
constexpr uint16_t kKeyBehaviorsMask       = 1u << 5;
constexpr uint16_t kVirtualModsMask        = 1u << 6;
constexpr uint16_t kVirtualModMapMask      = 1u << 7;
constexpr uint16_t kAllClientInfoMask      = 0x07;
constexpr uint16_t kAllServerInfoMask      = 0xF8;
constexpr uint16_t kAllMapComponentsMask   = 0xFF;

// GetNames `which`
constexpr uint32_t kKeycodesNameMask    = 1u << 0;
constexpr uint32_t kGeometryNameMask    = 1u << 1;
constexpr uint32_t kSymbolsNameMask     = 1u << 2;
constexpr uint32_t kPhysSymbolsNameMask = 1u << 3;
constexpr uint32_t kTypesNameMask       = 1u << 4;
constexpr uint32_t kCompatNameMask      = 1u << 5;
constexpr uint32_t kKeyTypeNamesMask    = 1u << 6;
constexpr uint32_t kKTLevelNamesMask    = 1u << 7;
constexpr uint32_t kIndicatorNamesMask  = 1u << 8;
constexpr uint32_t kKeyNamesMask        = 1u << 9;
constexpr uint32_t kKeyAliasesMask      = 1u << 10;
constexpr uint32_t kVirtualModNamesMask = 1u << 11;
constexpr uint32_t kGroupNamesMask      = 1u << 12;
constexpr uint32_t kRGNamesMask         = 1u << 13;
constexpr uint32_t kAllNamesMask        = 0x3fff;

// GetKbdByName want/need (XkbGBN_*)
constexpr uint16_t kGBN_TypesMask         = 1u << 0;
constexpr uint16_t kGBN_CompatMapMask     = 1u << 1;
constexpr uint16_t kGBN_ClientSymbolsMask = 1u << 2;
constexpr uint16_t kGBN_ServerSymbolsMask = 1u << 3;
constexpr uint16_t kGBN_IndicatorMapMask  = 1u << 4;
constexpr uint16_t kGBN_KeyNamesMask      = 1u << 5;
constexpr uint16_t kGBN_GeometryMask      = 1u << 6;
constexpr uint16_t kGBN_OtherNamesMask    = 1u << 7;
constexpr uint16_t kGBN_AllComponentsMask = 0xff;

// Real modifier bits (core state byte)
constexpr uint8_t kShiftMask   = 1u << 0;
constexpr uint8_t kLockMask    = 1u << 1;
constexpr uint8_t kControlMask = 1u << 2;
constexpr uint8_t kMod1Mask    = 1u << 3;
constexpr uint8_t kMod2Mask    = 1u << 4;
constexpr uint8_t kMod3Mask    = 1u << 5;
constexpr uint8_t kMod4Mask    = 1u << 6;
constexpr uint8_t kMod5Mask    = 1u << 7;

// Indicator map flags / which-mods
constexpr uint8_t kIM_UseBase      = 1u << 0;
constexpr uint8_t kIM_UseLatched   = 1u << 1;
constexpr uint8_t kIM_UseLocked    = 1u << 2;
constexpr uint8_t kIM_UseEffective = 1u << 3;
constexpr uint8_t kIM_UseCompat    = 1u << 4;
constexpr uint8_t kIM_LEDDrivesKB  = 1u << 5;
constexpr uint8_t kIM_NoAutomatic  = 1u << 6;
constexpr uint8_t kIM_NoExplicit   = 1u << 7;

// Boolean controls (enabledCtrls)
constexpr uint32_t kRepeatKeysMask      = 1u << 0;
constexpr uint32_t kSlowKeysMask        = 1u << 1;
constexpr uint32_t kBounceKeysMask      = 1u << 2;
constexpr uint32_t kStickyKeysMask      = 1u << 3;
constexpr uint32_t kMouseKeysMask       = 1u << 4;
constexpr uint32_t kMouseKeysAccelMask  = 1u << 5;
constexpr uint32_t kAccessXKeysMask     = 1u << 6;
constexpr uint32_t kAccessXTimeoutMask  = 1u << 7;
constexpr uint32_t kAccessXFeedbackMask = 1u << 8;
constexpr uint32_t kAudibleBellMask     = 1u << 9;
constexpr uint32_t kOverlay1Mask        = 1u << 10;
constexpr uint32_t kOverlay2Mask        = 1u << 11;
constexpr uint32_t kIgnoreGroupLockMask = 1u << 12;
constexpr uint32_t kAllBooleanCtrlsMask = 0x00001FFF;
constexpr uint32_t kAllControlsMask     = 0xF8001FFF;

// PerClientFlags
constexpr uint32_t kPCF_DetectableAutoRepeatMask = 1u << 0;
constexpr uint32_t kPCF_GrabsUseXKBStateMask     = 1u << 1;
constexpr uint32_t kPCF_AutoResetControlsMask    = 1u << 2;
constexpr uint32_t kPCF_LookupStateWhenGrabbed   = 1u << 3;
constexpr uint32_t kPCF_SendEventUsesXKBState    = 1u << 4;
constexpr uint32_t kPCF_AllFlagsMask             = 0x1f;

// Event selection masks (SelectEvents affectWhich)
constexpr uint16_t kNewKeyboardNotifyMask     = 1u << 0;
constexpr uint16_t kMapNotifyMask             = 1u << 1;
constexpr uint16_t kStateNotifyMask           = 1u << 2;
constexpr uint16_t kControlsNotifyMask        = 1u << 3;
constexpr uint16_t kIndicatorStateNotifyMask  = 1u << 4;
constexpr uint16_t kIndicatorMapNotifyMask    = 1u << 5;
constexpr uint16_t kNamesNotifyMask           = 1u << 6;
constexpr uint16_t kCompatMapNotifyMask       = 1u << 7;
constexpr uint16_t kBellNotifyMask            = 1u << 8;
constexpr uint16_t kActionMessageMask         = 1u << 9;
constexpr uint16_t kAccessXNotifyMask         = 1u << 10;
constexpr uint16_t kExtensionDeviceNotifyMask = 1u << 11;
constexpr uint16_t kAllEventsMask             = 0x0FFF;

// Per-event legal detail masks (XKB.h XkbAll*EventsMask)
constexpr uint32_t kAllNewKeyboardEventsMask     = 0x7;
constexpr uint32_t kAllStateEventsMask           = 0x3fff;
constexpr uint32_t kAllControlEventsMask         = kAllControlsMask;
constexpr uint32_t kAllIndicatorEventsMask       = 0xffffffffu;
constexpr uint32_t kAllNameEventsMask            = kAllNamesMask;
constexpr uint32_t kAllCompatMapEventsMask       = 0x3;
constexpr uint32_t kAllBellEventsMask            = 0x1;
constexpr uint32_t kAllActionMessagesMask        = 0x1;
constexpr uint32_t kAllAccessXEventsMask         = 0x7f;
constexpr uint32_t kAllExtensionDeviceEventsMask = 0x801f;

// XkbXI_* (GetDeviceInfo)
constexpr uint16_t kXI_KeyboardsMask         = 1u << 0;
constexpr uint16_t kXI_ButtonActionsMask     = 1u << 1;
constexpr uint16_t kXI_IndicatorNamesMask    = 1u << 2;
constexpr uint16_t kXI_IndicatorMapsMask     = 1u << 3;
constexpr uint16_t kXI_IndicatorStateMask    = 1u << 4;
constexpr uint16_t kXI_AllFeaturesMask       = 0x001f;
constexpr uint16_t kXI_AllDeviceFeaturesMask = 0x001e;

// Virtual modifier indices (xkeyboard-config order, so names match a stock
// xorg keymap; GDK looks up Meta/Super/Hyper by name).
constexpr uint8_t kVModNumLock    = 0;
constexpr uint8_t kVModAlt        = 1;
constexpr uint8_t kVModLevelThree = 2;
constexpr uint8_t kVModLAlt       = 3;
constexpr uint8_t kVModRAlt       = 4;
constexpr uint8_t kVModRControl   = 5;
constexpr uint8_t kVModLControl   = 6;
constexpr uint8_t kVModScrollLock = 7;
constexpr uint8_t kVModLevelFive  = 8;
constexpr uint8_t kVModAltGr      = 9;
constexpr uint8_t kVModMeta       = 10;
constexpr uint8_t kVModSuper      = 11;
constexpr uint8_t kVModHyper      = 12;
constexpr uint8_t kNumDefinedVMods = 13;

// Canonical key type indices (XkbOneLevelIndex..XkbKeypadIndex)
constexpr uint8_t kTypeOneLevel   = 0;
constexpr uint8_t kTypeTwoLevel   = 1;
constexpr uint8_t kTypeAlphabetic = 2;
constexpr uint8_t kTypeKeypad     = 3;

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
struct ModsDesc {
  uint8_t  mask = 0;      // effective real-modifier mask (what clients match on)
  uint8_t  realMods = 0;  // real modifiers named explicitly
  uint16_t vmods = 0;     // virtual modifiers (resolved into `mask` by us)
};

struct KeyTypeEntry {
  bool     active = true;
  ModsDesc mods;
  uint8_t  level = 0;
};

struct KeyType {
  ModsDesc mods;
  uint8_t  numLevels = 1;
  std::vector<KeyTypeEntry> map;
  std::vector<ModsDesc> preserve;         // empty = no preserve list on the wire
  uint32_t nameAtom = 0;
  std::vector<uint32_t> levelNameAtoms;   // exactly numLevels entries
};

struct KeyDesc {
  std::array<uint8_t, 4> ktIndex{};       // per-group key type (only [0] used)
  uint8_t  numGroups = 0;                 // 0 = unused keycode
  uint8_t  width = 0;                     // symbols per group
  std::vector<uint32_t> syms;             // width * numGroups keysyms, group-major
  uint8_t  modmap = 0;                    // real modifiers this key sets
  uint16_t vmodmap = 0;                   // virtual modifiers this key sets
  uint8_t  explicitMask = 0;
  bool     repeats = false;
  std::array<char, 4> name{};             // XKB key name, NUL padded
  // groupInfo byte: low nibble = group count, high bits = wrap-into-range (0).
  uint8_t groupInfo() const { return (uint8_t)(numGroups & 0x0f); }
  bool used() const { return numGroups != 0; }
};

struct IndicatorMapDesc {
  uint8_t  flags = 0;
  uint8_t  whichGroups = 0;
  uint8_t  groups = 0;
  uint8_t  whichMods = 0;
  ModsDesc mods;
  uint32_t ctrls = 0;
};

// Compat sym interpret (xkbSymInterpretWireDesc, 16 bytes).  We serve no key
// actions, so these only describe how a client compiling its own keymap
// would derive them — but xkbcomp refuses to write a keymap whose compat
// section has no interprets at all (XkbWriteXKBCompatMap), so a stock
// subset of xkeyboard-config's compat/basic + compat/misc is provided.
struct SymInterpret {
  uint32_t sym = 0;
  uint8_t  mods = 0;
  uint8_t  match = 0;        // XkbSI_* op | LevelOneOnly
  uint8_t  virtualMod = 0xff; // XkbNoModifier
  uint8_t  flags = 0;        // XkbSI_AutoRepeat / LockingKey
  std::array<uint8_t, 8> act{};   // xkbActionWireDesc
};

// SymInterpret.match ops and action types used here
constexpr uint8_t kSI_NoneOf = 0, kSI_AnyOfOrNone = 1, kSI_AnyOf = 2, kSI_AllOf = 3, kSI_Exactly = 4;
constexpr uint8_t kSI_LevelOneOnly = 0x80;
constexpr uint8_t kSA_NoAction = 0x00, kSA_SetMods = 0x01, kSA_LockMods = 0x03;
constexpr uint8_t kSA_ClearLocks = 0x01, kSA_UseModMapMods = 0x04;

// Keyboard controls (xkbGetControlsReply); values are xorg's defaults
// (xkb/xkbInit.c XkbInitControls, xkb/xkbAccessX.c AccessXInit).
struct Controls {
  uint8_t  mkDfltBtn = 1;
  uint8_t  numGroups = 1;
  uint8_t  groupsWrap = 1;                // XkbSetGroupInfo(1, WrapIntoRange, 0)
  ModsDesc internal;
  ModsDesc ignoreLock;
  uint16_t repeatDelay = 660;
  uint16_t repeatInterval = 40;
  uint16_t slowKeysDelay = 300;
  uint16_t debounceDelay = 300;
  uint16_t mkDelay = 160;
  uint16_t mkInterval = 40;
  uint16_t mkTimeToMax = 30;
  uint16_t mkMaxSpeed = 30;
  int16_t  mkCurve = 500;
  uint16_t axOptions = 0x0C2F;            // AllOptions minus IndicatorFB/SKReleaseFB/SKRejectFB
  uint16_t axTimeout = 120;
  uint16_t axtOptsMask = 0;
  uint16_t axtOptsValues = 0;
  uint32_t axtCtrlsMask = 0;
  uint32_t axtCtrlsValues = 0;
  uint32_t enabledCtrls = kAccessXTimeoutMask | kRepeatKeysMask |
                          kMouseKeysAccelMask | kAudibleBellMask |
                          kIgnoreGroupLockMask;
  std::array<uint8_t, 32> perKeyRepeat{};
};

class Keymap {
public:
  // The server's keyboard description, built lazily from the core tables.
  // Immutable after construction (the core tables are static too; a future
  // SetModifierMapping-driven rebuild would also have to send MapNotify /
  // core MappingNotify, see docs).
  static const Keymap& current();

  uint8_t minKeyCode = 8;
  uint8_t maxKeyCode = 255;
  uint16_t numKeys() const { return (uint16_t)(maxKeyCode - minKeyCode + 1); }
  bool inRange(unsigned kc) const { return kc >= minKeyCode && kc <= maxKeyCode; }

  std::vector<KeyType> types;
  std::array<KeyDesc, 256> keys{};

  uint16_t vmodDefined = 0;                       // bit i = vmod i has a name
  std::array<uint8_t, kNumVirtualMods> vmodRealMods{};
  std::array<uint32_t, kNumVirtualMods> vmodNameAtoms{};

  uint32_t physIndicators = 0;
  std::array<uint32_t, kNumIndicators> indicatorNameAtoms{};
  std::array<IndicatorMapDesc, kNumIndicators> indicatorMaps{};

  std::vector<SymInterpret> symInterprets;

  uint32_t keycodesNameAtom = 0, geometryNameAtom = 0, symbolsNameAtom = 0,
           physSymbolsNameAtom = 0, typesNameAtom = 0, compatNameAtom = 0;
  std::array<uint32_t, kNumKbdGroups> groupNameAtoms{};

  Controls controls;

  // Index of the indicator carrying `atom` as its name, or -1.
  int indicatorIndexForAtom(uint32_t atom) const;
};

// ---------------------------------------------------------------------------
// Wire builders.  Each fills `out` with one complete reply: 32-byte generic
// header (type=1, deviceID, seq, length) plus body, little-endian.
// ---------------------------------------------------------------------------
struct GetMapRequest {
  uint16_t deviceSpec = 0, full = 0, partial = 0;
  uint8_t  firstType = 0, nTypes = 0;
  uint8_t  firstKeySym = 0, nKeySyms = 0;
  uint8_t  firstKeyAct = 0, nKeyActs = 0;
  uint8_t  firstKeyBehavior = 0, nKeyBehaviors = 0;
  uint16_t virtualMods = 0;
  uint8_t  firstKeyExplicit = 0, nKeyExplicit = 0;
  uint8_t  firstModMapKey = 0, nModMapKeys = 0;
  uint8_t  firstVModMapKey = 0, nVModMapKeys = 0;
};

// xkbGetMapReply (40-byte header + sections).  Returns false with `errValue`
// (xorg _XkbErrCode2 encoding) when the request is malformed → BadValue.
bool buildGetMapReply(const Keymap& km, const GetMapRequest& rq, uint16_t seq,
                      uint8_t deviceID, std::vector<uint8_t>& out, uint32_t& errValue);

// xkbGetCompatMapReply.  Returns false → BadValue when the SI range is bad.
bool buildGetCompatMapReply(const Keymap& km, uint16_t seq, uint8_t deviceID,
                            uint8_t groups, bool getAllSI, uint16_t firstSI, uint16_t nSI,
                            std::vector<uint8_t>& out, uint32_t& errValue);

// xkbGetIndicatorMapReply: one 12-byte map per set bit of `which`.
void buildGetIndicatorMapReply(const Keymap& km, uint16_t seq, uint8_t deviceID,
                               uint32_t which, std::vector<uint8_t>& out);

// xkbGetNamesReply for `which` (already validated ⊆ kAllNamesMask).
void buildGetNamesReply(const Keymap& km, uint16_t seq, uint8_t deviceID,
                        uint32_t which, std::vector<uint8_t>& out);

// xkbGetControlsReply (92 bytes, length = 15).
void buildGetControlsReply(const Keymap& km, uint16_t seq, uint8_t deviceID,
                           std::vector<uint8_t>& out);

} // namespace x11::xkb
