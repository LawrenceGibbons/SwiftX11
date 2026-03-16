//
//  QueryOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Ops/QueryOps.hpp"
#include "Core/XClient.hpp"
#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/ByteReader.hpp"
#include "Core/XConstants.hpp"
#include "Utils/WireLE.hpp"
#include "Core/KeySyms.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/X11ExtOpcodes.hpp"

extern "C" {
#include "SwiftX11Bridge.h"
}

namespace x11 {

  QueryOps::QueryOps(XProtoRegistrar& reg) {
    reg.registerMajor(x11::opcode::QueryTree,          &QueryOps::onMajor, this); // QueryTree
    reg.registerMajor(x11::opcode::QueryPointer,       &QueryOps::onMajor, this); // QueryPointer
    reg.registerMajor(x11::opcode::GetInputFocus,      &QueryOps::onMajor, this); // GetInputFocus
    reg.registerMajor(x11::opcode::QueryColors,        &QueryOps::onMajor, this); // QueryColors
    reg.registerMajor(x11::opcode::QueryExtension,     &QueryOps::onMajor, this); // QueryExtension
    reg.registerMajor(x11::opcode::ListExtensions,     &QueryOps::onMajor, this); // ListExtensions
    reg.registerMajor(x11::opcode::GetKeyboardMapping, &QueryOps::onMajor, this); // GetKeyboardMapping
    reg.registerMajor(x11::opcode::TranslateCoords,   &QueryOps::onMajor, this); // 40
    reg.registerMajor(x11::opcode::WarpPointer,        &QueryOps::onMajor, this); // 41
    reg.registerMajor(x11::opcode::SetInputFocus,      &QueryOps::onMajor, this); // 42
    reg.registerMajor(x11::opcode::GetMotionEvents,   &QueryOps::onMajor, this); // 39
    reg.registerMajor(x11::opcode::QueryKeymap,       &QueryOps::onMajor, this); // 44

    // Extension opcodes
    reg.registerMajor(ext::kBigReq, &QueryOps::onMajor, this); // BIG-REQUESTS Enable
  }
  
  void QueryOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
    if (!user) { dc.br.skip(dc.br.remaining()); return; }
    static_cast<QueryOps*>(user)->handle(ctx, dc);
  }
  
  void QueryOps::handle(XProtoContext& ctx, DispatchContext& dc) {
    switch (dc.major) {
      case x11::opcode::QueryTree         : handleQueryTree(ctx, dc.seq, dc.br); return;
      case x11::opcode::QueryPointer      : handleQueryPointer(ctx, dc.seq, dc.br); return;
      case x11::opcode::GetInputFocus     : handleGetInputFocus(ctx, dc.seq, dc.br); return;
      case x11::opcode::QueryColors       : handleQueryColors(ctx, dc.seq, dc.br); return;
      case x11::opcode::QueryExtension    : handleQueryExtension(ctx, dc.seq, dc.br); return;
      case x11::opcode::ListExtensions    : handleListExtensions(ctx, dc.seq, dc.br); return;
      case x11::opcode::GetKeyboardMapping: handleGetKeyboardMapping(ctx, dc.seq, dc.br); return;
      case x11::opcode::TranslateCoords  : handleTranslateCoords(ctx, dc.seq, dc.br); return;
      case x11::opcode::WarpPointer      : handleWarpPointer(ctx, dc.seq, dc.br); return;
      case x11::opcode::SetInputFocus    : handleSetInputFocus(ctx, dc.seq, dc.minor, dc.br); return;
      case x11::opcode::GetMotionEvents : handleGetMotionEvents(ctx, dc.seq, dc.br); return;
      case x11::opcode::QueryKeymap     : handleQueryKeymap(ctx, dc.seq, dc.br); return;
      case ext::kBigReq                 : handleBigReqEnable(ctx, dc.seq, dc.br); return;
      default:
        dc.br.skip(dc.br.remaining());
        ctx.tracef("[QueryOps] unexpected major=%u\n", (unsigned)dc.major);
        return;
    }
  }
  
  // MARK: - Helpers
  // 4 keysyms per keycode: normal, shift, mode_switch, mode_switch+shift
  // Java Swing / GTK expect 4 columns; Mode_switch columns are NoSymbol on macOS.
  struct KeySyms4 { uint32_t syms[4]; };

  static constexpr uint8_t kKeysymsPerKeycode = 4;

  static inline uint8_t macToX11Keycode(uint8_t mac_vk) {
    return (uint8_t)(mac_vk + 8u);
  }

  static const KeySyms4* getUSMacKeymap4() {
    static KeySyms4 map[256];
    static bool inited = false;
    if (inited) return map;
    inited = true;

    // Default: NoSymbol for all columns
    for (auto &e : map) { e.syms[0] = e.syms[1] = e.syms[2] = e.syms[3] = XK_NoSymbol; }

    // Set a key with normal and shifted keysyms (columns 3-4 = NoSymbol)
    auto setMac = [&](uint8_t mac_vk, uint32_t lo, uint32_t hi) {
      const uint8_t kc = macToX11Keycode(mac_vk);
      map[kc].syms[0] = lo;
      map[kc].syms[1] = hi;
    };

    // Set a key with same keysym for normal and shifted
    auto setMac1 = [&](uint8_t mac_vk, uint32_t sym) {
      const uint8_t kc = macToX11Keycode(mac_vk);
      map[kc].syms[0] = sym;
      map[kc].syms[1] = sym;
    };

    // ---------- Letters (US layout) ----------
    setMac(0,  'a', 'A');   // kVK_ANSI_A
    setMac(1,  's', 'S');   // kVK_ANSI_S
    setMac(2,  'd', 'D');   // kVK_ANSI_D
    setMac(3,  'f', 'F');   // kVK_ANSI_F
    setMac(4,  'h', 'H');   // kVK_ANSI_H
    setMac(5,  'g', 'G');   // kVK_ANSI_G
    setMac(6,  'z', 'Z');   // kVK_ANSI_Z
    setMac(7,  'x', 'X');   // kVK_ANSI_X
    setMac(8,  'c', 'C');   // kVK_ANSI_C
    setMac(9,  'v', 'V');   // kVK_ANSI_V
    setMac(11, 'b', 'B');   // kVK_ANSI_B
    setMac(12, 'q', 'Q');   // kVK_ANSI_Q
    setMac(13, 'w', 'W');   // kVK_ANSI_W
    setMac(14, 'e', 'E');   // kVK_ANSI_E
    setMac(15, 'r', 'R');   // kVK_ANSI_R
    setMac(16, 'y', 'Y');   // kVK_ANSI_Y
    setMac(17, 't', 'T');   // kVK_ANSI_T
    setMac(31, 'o', 'O');   // kVK_ANSI_O
    setMac(32, 'u', 'U');   // kVK_ANSI_U
    setMac(34, 'i', 'I');   // kVK_ANSI_I
    setMac(35, 'p', 'P');   // kVK_ANSI_P
    setMac(37, 'l', 'L');   // kVK_ANSI_L
    setMac(38, 'j', 'J');   // kVK_ANSI_J
    setMac(40, 'k', 'K');   // kVK_ANSI_K
    setMac(45, 'n', 'N');   // kVK_ANSI_N
    setMac(46, 'm', 'M');   // kVK_ANSI_M

    // ---------- Digits & symbols ----------
    setMac(18, '1', '!');   // kVK_ANSI_1
    setMac(19, '2', '@');   // kVK_ANSI_2
    setMac(20, '3', '#');   // kVK_ANSI_3
    setMac(21, '4', '$');   // kVK_ANSI_4
    setMac(22, '6', '^');   // kVK_ANSI_6
    setMac(23, '5', '%');   // kVK_ANSI_5
    setMac(24, '=', '+');   // kVK_ANSI_Equal
    setMac(25, '9', '(');   // kVK_ANSI_9
    setMac(26, '7', '&');   // kVK_ANSI_7
    setMac(27, '-', '_');   // kVK_ANSI_Minus
    setMac(28, '8', '*');   // kVK_ANSI_8
    setMac(29, '0', ')');   // kVK_ANSI_0

    // ---------- Punctuation ----------
    setMac(30, ']', '}');   // kVK_ANSI_RightBracket
    setMac(33, '[', '{');   // kVK_ANSI_LeftBracket
    setMac(39, '\'', '"');  // kVK_ANSI_Quote
    setMac(41, ';', ':');   // kVK_ANSI_Semicolon
    setMac(42, '\\', '|'); // kVK_ANSI_Backslash
    setMac(43, ',', '<');   // kVK_ANSI_Comma
    setMac(44, '/', '?');   // kVK_ANSI_Slash
    setMac(47, '.', '>');   // kVK_ANSI_Period
    setMac(50, '`', '~');   // kVK_ANSI_Grave
    setMac(10, XK_section, XK_plusminus); // kVK_ISO_Section (§/±)

    // ---------- Whitespace / control ----------
    setMac1(36, XK_Return);    // kVK_Return
    setMac1(48, XK_Tab);       // kVK_Tab
    setMac1(49, XK_space);     // kVK_Space
    setMac1(51, XK_BackSpace); // kVK_Delete (backspace)
    setMac1(53, XK_Escape);    // kVK_Escape

    // ---------- Modifiers ----------
    setMac1(56, XK_Shift_L);    // kVK_Shift
    setMac1(60, XK_Shift_R);    // kVK_RightShift
    setMac1(59, XK_Control_L);  // kVK_Control
    setMac1(62, XK_Control_R);  // kVK_RightControl
    setMac1(58, XK_Alt_L);      // kVK_Option
    setMac1(61, XK_Alt_R);      // kVK_RightOption
    setMac1(55, XK_Super_L);    // kVK_Command
    setMac1(54, XK_Super_R);    // kVK_RightCommand
    setMac1(57, XK_Caps_Lock);  // kVK_CapsLock
    setMac1(63, XK_Meta_L);     // kVK_Function (Fn key → Meta_L)

    // ---------- Arrow keys ----------
    setMac1(123, XK_Left);   // kVK_LeftArrow
    setMac1(124, XK_Right);  // kVK_RightArrow
    setMac1(125, XK_Down);   // kVK_DownArrow
    setMac1(126, XK_Up);     // kVK_UpArrow

    // ---------- Navigation ----------
    setMac1(115, XK_Home);      // kVK_Home
    setMac1(117, XK_End);       // kVK_End
    setMac1(116, XK_Page_Up);   // kVK_PageUp
    setMac1(121, XK_Page_Down); // kVK_PageDown
    setMac1(119, XK_Delete);    // kVK_ForwardDelete
    setMac1(114, XK_Help);      // kVK_Help (Insert on PC keyboards)

    // ---------- Function keys ----------
    setMac1(122, XK_F1);   // kVK_F1
    setMac1(120, XK_F2);   // kVK_F2
    setMac1(99,  XK_F3);   // kVK_F3
    setMac1(118, XK_F4);   // kVK_F4
    setMac1(96,  XK_F5);   // kVK_F5
    setMac1(97,  XK_F6);   // kVK_F6
    setMac1(98,  XK_F7);   // kVK_F7
    setMac1(100, XK_F8);   // kVK_F8
    setMac1(101, XK_F9);   // kVK_F9
    setMac1(109, XK_F10);  // kVK_F10
    setMac1(103, XK_F11);  // kVK_F11
    setMac1(111, XK_F12);  // kVK_F12
    setMac1(105, XK_F13);  // kVK_F13
    setMac1(107, XK_F14);  // kVK_F14
    setMac1(113, XK_F15);  // kVK_F15
    setMac1(106, XK_F16);  // kVK_F16
    setMac1(64,  XK_F17);  // kVK_F17
    setMac1(79,  XK_F18);  // kVK_F18
    setMac1(80,  XK_F19);  // kVK_F19
    setMac1(90,  XK_F20);  // kVK_F20

    // ---------- Keypad ----------
    setMac1(82,  XK_KP_0);        // kVK_ANSI_Keypad0
    setMac1(83,  XK_KP_1);        // kVK_ANSI_Keypad1
    setMac1(84,  XK_KP_2);        // kVK_ANSI_Keypad2
    setMac1(85,  XK_KP_3);        // kVK_ANSI_Keypad3
    setMac1(86,  XK_KP_4);        // kVK_ANSI_Keypad4
    setMac1(87,  XK_KP_5);        // kVK_ANSI_Keypad5
    setMac1(88,  XK_KP_6);        // kVK_ANSI_Keypad6
    setMac1(89,  XK_KP_7);        // kVK_ANSI_Keypad7
    setMac1(91,  XK_KP_8);        // kVK_ANSI_Keypad8
    setMac1(92,  XK_KP_9);        // kVK_ANSI_Keypad9
    setMac1(65,  XK_KP_Decimal);  // kVK_ANSI_KeypadDecimal
    setMac1(67,  XK_KP_Multiply); // kVK_ANSI_KeypadMultiply
    setMac1(69,  XK_KP_Add);      // kVK_ANSI_KeypadPlus
    setMac1(71,  XK_Clear);       // kVK_ANSI_KeypadClear (NumLock on PC)
    setMac1(75,  XK_KP_Divide);   // kVK_ANSI_KeypadDivide
    setMac1(76,  XK_KP_Enter);    // kVK_ANSI_KeypadEnter
    setMac1(78,  XK_KP_Subtract); // kVK_ANSI_KeypadMinus
    setMac1(81,  XK_KP_Equal);    // kVK_ANSI_KeypadEquals

    return map;
  }
  
  // MARK: - Handlers
  // ---- 43: GetInputFocus ----
  void QueryOps::handleGetInputFocus(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    br.skip(br.remaining());
    const uint32_t f = ctx.input().focus_xid ? ctx.input().focus_xid : 0;
    (void)ctx.reply().sendGetInputFocusReply(seq, /*revertTo*/0, /*focus*/f);
  }
  
  // ---- 38: QueryPointer ----
  void QueryOps::handleQueryPointer(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t qwin = br.readU32();
    br.skip(br.remaining());

    // Validate window exists (root is always valid)
    if (qwin != kRootXid && qwin != 0) {
      WindowView tmp{};
      if (!ctx.windows().snapshot(qwin, tmp)) {
        ctx.transport().sendErrorCore(x11::error::BadWindow, seq, qwin, x11::opcode::QueryPointer);
        return;
      }
    }

    const auto& in = ctx.input();
    
    // Root coords (global, top-left)
    const int32_t rootx32 = in.root_x_u;
    const int32_t rooty32 = in.root_y_u;
    const uint16_t mask = (uint16_t)(in.buttons | in.mods);
    
    // Host-local coords (relative to host_xid view)
    const uint32_t host = in.last_xid;
    const int32_t hostx = in.win_x_u;
    const int32_t hosty = in.win_y_u;
    
    auto clamp16 = [](int32_t v) -> int16_t {
      if (v < -32768) return -32768;
      if (v >  32767) return  32767;
      return (int16_t)v;
    };
    
    const int16_t rootx = clamp16(rootx32);
    const int16_t rooty = clamp16(rooty32);
    
    // Default child/win coords
    uint32_t child = 0;
    int16_t winx = 0;
    int16_t winy = 0;
    
    // If we don't know host yet, answer root-only.
    if (host == 0) {
      (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
        rep[1] = 1;
        wire::wr32_le(rep.data() + 8,  kRootXid);
        wire::wr32_le(rep.data() + 12, 0);
        wire::wr16_le(rep.data() + 16, (uint16_t)rootx);
        wire::wr16_le(rep.data() + 18, (uint16_t)rooty);
        wire::wr16_le(rep.data() + 20, 0);
        wire::wr16_le(rep.data() + 22, 0);
        wire::wr16_le(rep.data() + 24, mask);
      });
      return;
    }
    
    // --- Helper: find deepest mapped child under pointer (no mask requirement) ---
    auto pick_deepest_mapped_child = [&](uint32_t host_xid, int32_t x, int32_t y) -> uint32_t {
      uint32_t best = host_xid;
      int bestDepth = -1;
      
      std::vector<uint32_t> nodes = ctx.windows().descendantsOf(host_xid);
      nodes.push_back(host_xid);
      
      auto contains = [&](uint32_t xid, const WindowView& vw, int32_t& lx, int32_t& ly, int& depth) -> bool {
        lx = x;
        ly = y;
        depth = 0;
        uint32_t cur = xid;
        while (cur && cur != host_xid) {
          WindowView cv{};
          if (!ctx.windows().snapshot(cur, cv)) return false;
          lx -= cv.x;
          ly -= cv.y;
          cur = cv.parent_xid;
          depth++;
          if (depth > 64) return false;
        }
        if (xid != host_xid && cur != host_xid) return false;
        return (lx >= 0 && ly >= 0 && lx < (int32_t)vw.w && ly < (int32_t)vw.h);
      };
      
      for (uint32_t xid : nodes) {
        WindowView vw{};
        if (!ctx.windows().snapshot(xid, vw)) continue;
        if (!vw.mapped) continue;
        
        int32_t lx=0, ly=0; int depth=0;
        if (!contains(xid, vw, lx, ly, depth)) continue;
        
        if (depth > bestDepth) {
          best = xid;
          bestDepth = depth;
        }
      }
      return best;
    };
    
    // --- Compute winX/winY relative to qwin ---
    if (qwin == kRootXid) {
      // When querying the root, window coords are root coords
      winx = rootx;
      winy = rooty;
      // child: direct child of root that pointer is in = the current host
      child = host;
    } else {
      // Determine which host tree qwin belongs to
      const uint32_t qwin_host = ctx.windows().topLevelAncestorOf(qwin);

      if (qwin_host == host) {
        // Same host as pointer — use host-local coords (fast path)
        child = pick_deepest_mapped_child(host, hostx, hosty);
        if (child == host) child = 0;

        int32_t lx = hostx;
        int32_t ly = hosty;

        uint32_t cur = qwin;
        int depth = 0;
        while (cur && cur != host) {
          WindowView cv{};
          if (!ctx.windows().snapshot(cur, cv)) { cur = 0; break; }
          lx -= cv.x;
          ly -= cv.y;
          cur = cv.parent_xid;
          depth++;
          if (depth > 64) { cur = 0; break; }
        }

        if (cur == host || qwin == host) {
          winx = clamp16(lx);
          winy = clamp16(ly);
        }
      } else {
        // Different host than pointer — use root coords + cached host screen origin.
        // This handles the cross-host case (e.g. xeyes querying its own window
        // while the pointer is over xterm's window).
        child = 0; // pointer is not in qwin's host tree

        const uint32_t lookup_host = (qwin_host != 0) ? qwin_host : qwin;
        int32_t hox = 0, hoy = 0;
        if (in.getHostOrigin(lookup_host, hox, hoy)) {
          // Walk from qwin up to its host to get child offset within host
          int32_t child_ox = 0, child_oy = 0;
          if (qwin != lookup_host) {
            uint32_t cur = qwin;
            for (int hop = 0; hop < 64 && cur && cur != lookup_host && cur != kRootXid; hop++) {
              WindowView cv{};
              if (!ctx.windows().snapshot(cur, cv)) break;
              child_ox += (int32_t)cv.x;
              child_oy += (int32_t)cv.y;
              cur = cv.parent_xid;
            }
          }
          // qwin's screen position = host_screen_origin + child_offset_in_host
          // window-local coords = root_position - qwin_screen_position
          winx = clamp16(rootx32 - hox - child_ox);
          winy = clamp16(rooty32 - hoy - child_oy);
        }
        // else: host never visited, winx/winy stay at 0
      }
    }
    
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = 1; // sameScreen
      wire::wr32_le(rep.data() + 8,  kRootXid);
      wire::wr32_le(rep.data() + 12, child);
      wire::wr16_le(rep.data() + 16, (uint16_t)rootx);
      wire::wr16_le(rep.data() + 18, (uint16_t)rooty);
      wire::wr16_le(rep.data() + 20, (uint16_t)winx);
      wire::wr16_le(rep.data() + 22, (uint16_t)winy);
      wire::wr16_le(rep.data() + 24, mask);
    });
  }
  
  
  // ---- 15: QueryTree ----
  void QueryOps::handleQueryTree(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t wid = br.readU32();
    br.skip(br.remaining());
    
    // root is always constant in your server
    static constexpr uint32_t kRootXid = 0x00000001u;
    
    uint32_t parent = 0;
    uint32_t children[256];
    uint32_t nchildren = 0;
    
    // Use authoritative WindowTable (no C bridge)
    bool ok = ctx.windows().queryTree(wid, &parent, children, 256, &nchildren);
    if (!ok) {
      // Root window (XID 1) is always valid even though it's not in WindowTable
      if (wid != kRootXid && wid != 0) {
        ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::QueryTree);
        return;
      }
      parent = 0;
      nchildren = 0;
    }
    
    const uint32_t extra_words = nchildren; // each child is CARD32
    
    // Reply header (32 bytes)
    const bool okHdr = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      wire::wr32_le(rep.data() + 4, extra_words);   // length_words
      wire::wr32_le(rep.data() + 8, kRootXid);      // root
      wire::wr32_le(rep.data() + 12, parent);       // parent
      wire::wr16_le(rep.data() + 16, (uint16_t)nchildren);
    });
    if (!okHdr) return;
    
    // Children payload: CARD32[] little-endian
    if (nchildren) {
      uint8_t out[256 * 4];
      for (uint32_t i = 0; i < nchildren; i++) {
        wire::wr32_le(out + (size_t)i * 4u, children[i]);
      }
      // Already 4-byte aligned, so sendReplyBytes is fine (no padding needed)
      (void)ctx.transport().sendReplyBytes(out, (std::size_t)nchildren * 4u);
    }
  }
  
  
  
  // ---- 91: QueryColors ----
  //
  // Request body (per XCB + working legacy code):
  //   CARD32 colormap
  //   LISTofCARD32 pixels     // count implied by request length
  //
  // Reply header includes:
  //   bytes 8..9: CARD16 nColors
  // Reply payload:
  //   nColors * { CARD16 red, CARD16 green, CARD16 blue, CARD16 pad }
  //
  // 91 QueryColors (reply)
  // req: cmap(CARD32), n(CARD16), pad(CARD16), pixels[n] (CARD32)
  // reply: length_words = n * sz_xColorItem /4, payload list of xColorItem
  // ---- 91: QueryColors ----
  //
  // Request body (per XCB + working legacy code):
  //   CARD32 colormap
  //   LISTofCARD32 pixels     // count implied by request length
  //
  // Reply header includes:
  //   bytes 8..9: CARD16 nColors
  // Reply payload:
  //   nColors * { CARD16 red, CARD16 green, CARD16 blue, CARD16 pad }
  //
  // ---- 91: QueryColors ----
  // Request (core X11):
  //   CARD32 colormap
  //   LISTofCARD32 pixels   // count implied by request length
  //
  // Reply:
  //   CARD16 nColors (in reply header bytes 8..9)
  //   nColors * xrgb (8 bytes each): CARD16 red, green, blue, pad
  static constexpr uint32_t kRootCmap = 0x00000020u; // default colormap advertised in setup

  void QueryOps::handleQueryColors(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }

    const uint32_t cmap = br.readU32();

    // Validate colormap (only root colormap supported in QueryOps)
    if (cmap != kRootCmap) {
      br.skip(br.remaining());
      ctx.transport().sendErrorCore(x11::error::BadColor, seq, cmap, x11::opcode::QueryColors);
      return;
    }

    uint16_t ncolors = (uint16_t)(br.remaining() / 4u);
    if (ncolors > 4096) ncolors = 4096; // avoid absurd allocations

    const uint32_t extra_words = (uint32_t)ncolors * 2u; // 8 bytes/item => 2 words/item

  #ifndef NDEBUG
    ctx.tracef("[QueryColors] seq=%u n=%u extra_words=%u req_remain=%zu\n",
               (unsigned)seq, (unsigned)ncolors, (unsigned)extra_words, br.remaining());
  #endif

    const bool ok = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      wire::wr32_le(rep.data() + 4, extra_words);      // reply length in 4-byte units
      wire::wr16_le(rep.data() + 8, ncolors);          // nColors
    });
    if (!ok) return;

    auto toRGB16 = [](uint32_t px, uint16_t& r, uint16_t& g, uint16_t& b) {
      // Many clients use 0x00RRGGBB in the pixel list for TrueColor colormaps.
      const uint8_t rr = (uint8_t)((px >> 16) & 0xFF);
      const uint8_t gg = (uint8_t)((px >>  8) & 0xFF);
      const uint8_t bb = (uint8_t)((px >>  0) & 0xFF);
      r = (uint16_t(rr) << 8) | rr;
      g = (uint16_t(gg) << 8) | gg;
      b = (uint16_t(bb) << 8) | bb;
    };

    uint8_t out[8];

    for (uint16_t i = 0; i < ncolors; i++) {
      const uint32_t px = br.readU32();

      uint16_t r=0, g=0, b=0;
      toRGB16(px, r, g, b);

      wire::wr16_le(out + 0, r);
      wire::wr16_le(out + 2, g);
      wire::wr16_le(out + 4, b);
      wire::wr16_le(out + 6, 0); // pad

      (void)ctx.reply().sendPaddedBytes(out, sizeof(out)); // 8 bytes
    }

    // Defensive: consume any trailing bytes
    br.skip(br.remaining());
  }
  
  // ---- 98: QueryExtension ----
  //
  // Request body:
  //   CARD16 nbytes
  //   CARD16 pad
  //   LISTofCHAR name (nbytes), followed by padding to 4-byte multiple
  //
  // Reply body:
  //   BYTE present
  //   CARD8 major_opcode
  //   CARD8 first_event
  //   CARD8 first_error
  //   length = 0 (no extra data)
  void QueryOps::handleQueryExtension(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }

    const uint16_t nbytes = br.readU16();
    br.skip(2); // pad

    // Read the extension name
    std::string name;
    const std::size_t avail = br.remaining();
    const std::size_t take  = std::min<std::size_t>(nbytes, avail);
    if (take > 0) {
      name.assign(reinterpret_cast<const char*>(br.ptr()), take);
      br.skip(take);
    }
    br.skip(br.remaining()); // consume padding + rest

    // Check supported extensions
    uint8_t present = 0, major = 0, first_event = 0, first_error = 0;

    if (name == "BIG-REQUESTS") {
      present = 1; major = ext::kBigReq;
    } else if (name == "RENDER") {
      present = 1; major = ext::kRENDER;
    } else if (name == "XFIXES") {
      present = 1; major = ext::kXFIXES;
      first_event = ext::kXFIXES_FirstEvent;
    } else if (name == "RANDR") {
      present = 1; major = ext::kRANDR;
      first_event = ext::kRANDR_FirstEvent;
    } else if (name == "XINERAMA" || name == "PANORAMIX") {
      present = 1; major = ext::kXinerama;
    } else if (name == "Generic Event Extension") {
      present = 1; major = ext::kGE;
      first_event = ext::kGE_FirstEvent;
    } else if (name == "SHAPE") {
      present = 1; major = ext::kSHAPE;
      first_event = ext::kSHAPE_FirstEvent;
    } else if (name == "XC-MISC") {
      present = 1; major = ext::kXCMisc;
    }

#ifndef NDEBUG
    fprintf(stderr, "[QueryExtension] \"%s\" -> present=%u major=%u\n",
            name.c_str(), (unsigned)present, (unsigned)major);
#endif

    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      wire::wr32_le(rep.data() + 4, 0); // length = 0
      rep[8]  = present;
      rep[9]  = major;
      rep[10] = first_event;
      rep[11] = first_error;
    });
  }
  
  // ---- 99: ListExtensions ----
  //
  // Reply:
  //   BYTE nExtensions (rep[1])
  //   length in 4-byte words of additional data
  //   Payload: sequence of STR (1-byte length + name bytes), padded to 4 bytes
  void QueryOps::handleListExtensions(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    br.skip(br.remaining()); // request has no extra fields we care about

    // List extensions that are fully (or minimally) functional.
    static const char* extensions[] = {
      "BIG-REQUESTS",
      "RENDER",
      "XFIXES",
      "RANDR",
      "XINERAMA",
      "Generic Event Extension",
      "SHAPE",
      "XC-MISC",
    };
    static constexpr uint8_t nExt = 8;

    // Build payload: each entry is 1-byte length + name bytes (no per-entry padding)
    std::vector<uint8_t> payload;
    for (uint8_t i = 0; i < nExt; i++) {
      const size_t len = std::strlen(extensions[i]);
      payload.push_back((uint8_t)len);
      payload.insert(payload.end(), extensions[i], extensions[i] + len);
    }
    // Pad to 4-byte boundary
    while (payload.size() % 4u) payload.push_back(0);

    const uint32_t lenWords = (uint32_t)(payload.size() / 4u);

    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      wire::wr32_le(rep.data() + 4, lenWords);
      rep[1] = nExt;
    });
    if (!payload.empty()) {
      ctx.reply().sendBytes(payload.data(), payload.size());
    }
  }


  // ---- 133: BigReqEnable (BIG-REQUESTS extension, opcode 0) ----
  //
  // Request: just the 4-byte header (no additional data)
  // Reply:
  //   CARD32 maximum-request-length (in 4-byte units)
  //
  // After this reply, the client can send requests with len_words==0
  // followed by a 32-bit extended length.
  void QueryOps::handleBigReqEnable(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    br.skip(br.remaining());

    // Enable BIG-REQUESTS for this client
    if (ctx.hasClient() && ctx.client()) {
      ctx.client()->setBigReqEnabled(true);
    }

    // Maximum request length: 1M words = 4MB
    static constexpr uint32_t kMaxBigReqWords = 0x00100000u; // 1048576 words = 4MB

#ifndef NDEBUG
    fprintf(stderr, "[BigReqEnable] enabled, max_request_length=%u words\n",
            (unsigned)kMaxBigReqWords);
#endif

    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      wire::wr32_le(rep.data() + 4, 0); // length=0 (no extra data)
      wire::wr32_le(rep.data() + 8, kMaxBigReqWords);
    });
  }

  // ---- 101: GetKeyboardMapping ----
  void QueryOps::handleGetKeyboardMapping(XProtoContext& ctx, uint16_t seq, ByteReader& br)
  {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }

    const uint8_t first = br.readU8();
    const uint8_t count = br.readU8();
    br.skip(2);
    br.skip(br.remaining());

    const uint32_t nSyms = (uint32_t)count * (uint32_t)kKeysymsPerKeycode;
    const uint32_t payloadBytes = nSyms * 4u;
    const uint32_t payloadWords = payloadBytes / 4u;

    std::vector<uint8_t> payload(payloadBytes, 0);

    const KeySyms4* map = getUSMacKeymap4();

    for (uint32_t i = 0; i < count; i++) {
      const uint8_t kc = (uint8_t)(first + (uint8_t)i);
      const KeySyms4& ks = map[kc];

      uint8_t* out = payload.data() + i * (kKeysymsPerKeycode * 4u);
      for (uint8_t col = 0; col < kKeysymsPerKeycode; col++) {
        wire::wr32_le(out + col * 4u, ks.syms[col]);
      }
    }

    const bool ok = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = kKeysymsPerKeycode;
      wire::wr32_le(rep.data() + 4, payloadWords);
    });
    if (!ok) return;

    if (!payload.empty()) {
      (void)ctx.reply().sendBytes(payload.data(), payload.size());
    }
  }

// ---- 40: TranslateCoords ----
// Body: srcWin(4), dstWin(4), srcX(2), srcY(2)
// Reply: sameScreen, child, dstX, dstY
void QueryOps::handleTranslateCoords(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }
  const uint32_t srcWin = br.readU32();
  const uint32_t dstWin = br.readU32();
  const int16_t srcX = br.readI16();
  const int16_t srcY = br.readI16();
  br.skip(br.remaining());

  // Validate srcWin exists (root is always valid)
  if (srcWin != kRootXid && srcWin != 0) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(srcWin, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, srcWin, x11::opcode::TranslateCoords);
      return;
    }
  }
  // Validate dstWin exists (root is always valid)
  if (dstWin != kRootXid && dstWin != 0) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(dstWin, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, dstWin, x11::opcode::TranslateCoords);
      return;
    }
  }

  // Compute absolute (root) position of point in src window.
  // X11 spec: a window's content origin is at (x + border_width, y + border_width)
  // in parent coordinates.  We must include border_width at each level.
  int32_t absX = (int32_t)srcX;
  int32_t absY = (int32_t)srcY;
  {
    uint32_t cur = srcWin;
    for (int hop = 0; hop < 256 && cur && cur != kRootXid; hop++) {
      WindowView vw{};
      if (!ctx.windows().snapshot(cur, vw)) break;
      absX += (int32_t)vw.x + (int32_t)vw.border_width;
      absY += (int32_t)vw.y + (int32_t)vw.border_width;
      cur = vw.parent_xid;
    }
  }

  // Convert to dst window local coords
  int32_t dstX = absX;
  int32_t dstY = absY;
  if (dstWin != kRootXid) {
    uint32_t cur = dstWin;
    for (int hop = 0; hop < 256 && cur && cur != kRootXid; hop++) {
      WindowView vw{};
      if (!ctx.windows().snapshot(cur, vw)) break;
      dstX -= (int32_t)vw.x + (int32_t)vw.border_width;
      dstY -= (int32_t)vw.y + (int32_t)vw.border_width;
      cur = vw.parent_xid;
    }
  }

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = 1; // sameScreen
    wire::wr32_le(rep.data() + 8, 0); // child = None
    wire::wr16_le(rep.data() + 12, (uint16_t)(int16_t)dstX);
    wire::wr16_le(rep.data() + 14, (uint16_t)(int16_t)dstY);
  });
}

// ---- 41: WarpPointer ----
void QueryOps::handleWarpPointer(XProtoContext& ctx, uint16_t /*seq*/, ByteReader& br) {
  // Body (20 bytes):
  //   CARD32 srcWindow (0=None), CARD32 dstWindow (0=None)
  //   INT16 srcX, srcY, CARD16 srcWidth, srcHeight
  //   INT16 dstX, dstY
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t srcWin  = br.readU32();
  const uint32_t dstWin  = br.readU32();
  /*srcX*/ br.readI16(); /*srcY*/ br.readI16();
  /*srcW*/ br.readU16(); /*srcH*/ br.readU16();
  const int16_t  dstX    = br.readI16();
  const int16_t  dstY    = br.readI16();
  br.skip(br.remaining());

  (void)srcWin; // TODO: honour src-window constraint

  // UI queue convention:
  //   xid == 0: relative warp (x_u/y_u are deltas from current pointer)
  //   xid != 0: absolute warp in host-window-local coordinates
  static constexpr uint32_t kRootXid = 0x00000029u;

  if (dstWin == 0) {
    // Relative warp
    x11_ui_push_warp_pointer(0, (int32_t)dstX, (int32_t)dstY);
  } else {
    // Window-relative → host-local coordinates
    uint32_t target = (dstWin == kRootXid) ? 0 : dstWin;
    uint32_t host = 0;
    int32_t offX = 0, offY = 0;
    if (target != 0) {
      host = ctx.windows().topLevelAncestorOf(target);
      if (host == 0) host = target;
      ctx.windows().absoluteOffsetInHost(host, target, offX, offY);
    }
    // host==0 means root-relative (treated as screen coordinates by Swift)
    x11_ui_push_warp_pointer(host, (int32_t)dstX + offX, (int32_t)dstY + offY);
  }
}

// ---- 42: SetInputFocus ----
// Body: focus(4), time(4)  (revertTo is in dc.minor)
void QueryOps::handleSetInputFocus(XProtoContext& ctx, uint16_t seq, uint8_t revertTo, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }
  const uint32_t focus = br.readU32();
  (void)br.readU32(); // time
  br.skip(br.remaining());

  // Validate focus window: None (0) and PointerRoot (1) are always valid
  if (focus != 0 && focus != 1 && focus != kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(focus, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, focus, x11::opcode::SetInputFocus);
      return;
    }
  }

  const uint32_t oldFocus = ctx.input().focus_xid;
  const uint32_t newFocus = (focus == 0) ? 0 : (focus == 1) ? kRootXid : focus;

  // Send FocusOut to old focus window
  if (oldFocus != 0 && oldFocus != newFocus) {
    uint8_t ev[32] = {};
    ev[0] = 10; // FocusOut
    ev[1] = 0;  // detail = Ancestor
    wire::wr16_le(ev + 2, 0);
    wire::wr32_le(ev + 4, oldFocus);
    ev[8] = 0; // mode = Normal
    (void)ctx.transport().sendEvent32(oldFocus, ev);
  }

  // Update focus state
  ctx.input().focus_xid = newFocus;
  if (newFocus != 0 && newFocus != kRootXid) {
    ctx.input().focus_host = ctx.windows().topLevelAncestorOf(newFocus);
  }

  // Send FocusIn to new focus window
  if (newFocus != 0) {
    uint8_t ev[32] = {};
    ev[0] = 9; // FocusIn
    ev[1] = 0; // detail = Ancestor
    wire::wr16_le(ev + 2, 0);
    wire::wr32_le(ev + 4, newFocus);
    ev[8] = 0; // mode = Normal
    (void)ctx.transport().sendEvent32(newFocus, ev);
  }

  (void)revertTo; // stored but unused for now

#ifndef NDEBUG
  fprintf(stderr, "[SetInputFocus] old=0x%08X new=0x%08X revertTo=%u\n",
          (unsigned)oldFocus, (unsigned)newFocus, (unsigned)revertTo);
#endif
}

// ---- 39: GetMotionEvents (stub: empty list) ----
void QueryOps::handleGetMotionEvents(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  br.skip(br.remaining());
  (void)ctx.reply().sendReply32(seq, [](std::array<uint8_t, 32>& rep) {
    // rep[8..11] = nEvents = 0 (already zeroed)
  });
}

// ---- 44: QueryKeymap (stub: all keys up) ----
void QueryOps::handleQueryKeymap(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  br.skip(br.remaining());
  // Reply: 32-byte header + 8 extra bytes = 40 bytes total.
  // Keymap occupies rep[8..31] (24 bytes) + 8 extra bytes = 32 bytes of keymap.
  std::array<uint8_t, 40> rep{};
  rep[0] = 1; // Reply
  rep[2] = (uint8_t)(seq & 0xFF);
  rep[3] = (uint8_t)((seq >> 8) & 0xFF);
  rep[4] = 2; // length in 4-byte units (8 extra bytes / 4)
  // Copy real key state into bytes 8..39 (32 bytes = 256 bits)
  std::memcpy(rep.data() + 8, ctx.input().getKeymap(), 32);
  (void)ctx.reply().sendReplyRaw(rep.data(), rep.size());
}

} // namespace x11
