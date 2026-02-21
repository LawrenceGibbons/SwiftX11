//
//  QueryOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstdio>
#include <vector>

#include "Ops/QueryOps.hpp"
#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/ByteReader.hpp"
#include "Core/XConstants.hpp"
#include "Utils/WireLE.hpp"
#include "Core/KeySyms.hpp"
#include "Core/X11CoreOpcodes.hpp"

namespace x11 {
  
  QueryOps::QueryOps(XProtoRegistrar& reg) {
    reg.registerMajor(x11::opcode::QueryTree,          &QueryOps::onMajor, this); // QueryTree
    reg.registerMajor(x11::opcode::QueryPointer,       &QueryOps::onMajor, this); // QueryPointer
    reg.registerMajor(x11::opcode::GetInputFocus,      &QueryOps::onMajor, this); // GetInputFocus
    reg.registerMajor(x11::opcode::QueryColors,        &QueryOps::onMajor, this); // QueryColors
    reg.registerMajor(x11::opcode::QueryExtension,     &QueryOps::onMajor, this); // QueryExtension
    reg.registerMajor(x11::opcode::ListExtensions,     &QueryOps::onMajor, this); // ListExtensions
    reg.registerMajor(x11::opcode::GetKeyboardMapping, &QueryOps::onMajor, this); // GetKeyboardMapping
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
      default: 
        dc.br.skip(dc.br.remaining());
        ctx.tracef("[QueryOps] unexpected major=%u\n", (unsigned)dc.major);
        return;
    }
  }
  
  // MARK: - Helpers
  struct KeySyms2 { uint32_t lo; uint32_t hi; }; // unshifted, shifted
  
  static inline uint8_t macToX11Keycode(uint8_t mac_vk) {
    // Your current detail mapping uses mac_vk + 8
    return (uint8_t)(mac_vk + 8u);
  }
  
  static const KeySyms2* getUSMacKeymap2() {
    static KeySyms2 map[256];
    static bool inited = false;
    if (inited) return map;
    inited = true;
    
    // Default: NoSymbol
    for (auto &e : map) { e.lo = XK_NoSymbol; e.hi = XK_NoSymbol; }
    
    auto setMac = [&](uint8_t mac_vk, uint32_t lo, uint32_t hi) {
      const uint8_t kc = macToX11Keycode(mac_vk);
      map[kc].lo = lo;
      map[kc].hi = hi;
    };
    
    // Letters (US layout)
    setMac(0,  'a', 'A');
    setMac(1,  's', 'S');
    setMac(2,  'd', 'D');
    setMac(3,  'f', 'F');
    setMac(4,  'h', 'H');
    setMac(5,  'g', 'G');
    setMac(6,  'z', 'Z');
    setMac(7,  'x', 'X');
    setMac(8,  'c', 'C');
    setMac(9,  'v', 'V');
    setMac(11, 'b', 'B');
    setMac(12, 'q', 'Q');
    setMac(13, 'w', 'W');
    setMac(14, 'e', 'E');
    setMac(15, 'r', 'R');
    setMac(16, 'y', 'Y');
    setMac(17, 't', 'T');
    setMac(31, 'o', 'O');
    setMac(32, 'u', 'U');
    setMac(34, 'i', 'I');
    setMac(35, 'p', 'P');
    setMac(37, 'l', 'L');
    setMac(38, 'j', 'J');
    setMac(40, 'k', 'K');
    setMac(45, 'n', 'N');
    setMac(46, 'm', 'M');
    
    // Digits & symbols
    setMac(18, '1', '!');
    setMac(19, '2', '@');
    setMac(20, '3', '#');
    setMac(21, '4', '$');
    setMac(22, '6', '^');
    setMac(23, '5', '%');
    setMac(24, '=', '+');
    setMac(25, '9', '(');
    setMac(26, '7', '&');
    setMac(27, '-', '_');
    setMac(28, '8', '*');
    setMac(29, '0', ')');
    
    // Punctuation
    setMac(30, ']', '}');
    setMac(33, '[', '{');
    setMac(39, '\'', '"');
    setMac(41, ';', ':');
    setMac(42, '\\', '|');
    setMac(43, ',', '<');
    setMac(44, '/', '?');
    setMac(47, '.', '>');
    setMac(50, '`', '~');
    
    // Whitespace / control
    setMac(36, XK_Return, XK_Return);
    setMac(48, XK_Tab, XK_Tab);
    setMac(49, XK_space, XK_space);
    setMac(51, XK_BackSpace, XK_BackSpace);
    setMac(53, XK_Escape, XK_Escape);
    
    // Modifiers (keysyms; keycodes are used for GetModifierMapping)
    setMac(56, XK_Shift_L, XK_Shift_L);
    setMac(60, XK_Shift_R, XK_Shift_R);
    setMac(59, XK_Control_L, XK_Control_L);
    setMac(62, XK_Control_R, XK_Control_R);
    setMac(58, XK_Alt_L, XK_Alt_L);
    setMac(61, XK_Alt_R, XK_Alt_R);
    setMac(55, XK_Super_L, XK_Super_L);   // Command_L
    setMac(54, XK_Super_R, XK_Super_R);   // Command_R (if you ever emit it)
    setMac(57, XK_Caps_Lock, XK_Caps_Lock);
    
    // Arrow keys
    setMac(123, XK_Left, XK_Left);
    setMac(124, XK_Right, XK_Right);
    setMac(125, XK_Down, XK_Down);
    setMac(126, XK_Up, XK_Up);
    
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
    
    child = pick_deepest_mapped_child(host, hostx, hosty);
    if (child == host) {
      // If host contains pointer, child should be 0 unless a true subwindow exists.
      // We'll leave host here for bring-up; change to 0 later if you want strictness.
    }
    
    // --- Compute winX/winY relative to qwin ---
    if (qwin == kRootXid) {
      // When querying the root, window coords are root coords
      winx = rootx;
      winy = rooty;
    } else {
      // Compute qwin local coords using host-local pointer coords.
      // Walk from qwin up to host, accumulating offsets; if not under host, leave winx/winy=0.
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
        // Clamp to qwin bounds if we can snapshot it
        WindowView qw{};
        if (ctx.windows().snapshot(qwin, qw)) {
          if (lx < 0) lx = 0;
          if (ly < 0) ly = 0;
          if (lx > (int32_t)qw.w) lx = (int32_t)qw.w;
          if (ly > (int32_t)qw.h) ly = (int32_t)qw.h;
        }
        winx = clamp16(lx);
        winy = clamp16(ly);
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
  void QueryOps::handleQueryColors(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }

    (void)br.readU32(); // cmap (ignored for bring-up)

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
  //
  // Bring-up: we report present=0 for all names (no extensions supported).
  void QueryOps::handleQueryExtension(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    
    const uint16_t nbytes = br.readU16();
    br.skip(2); // pad
    
    // Consume name bytes (if present) and 4-byte pad.
    const std::size_t avail = br.remaining();
    const std::size_t take  = std::min<std::size_t>(nbytes, avail);
    br.skip(take);
    // Skip any remaining padding (request is padded to 4-byte boundary).
    const std::size_t rem = br.remaining();
    const std::size_t pad = rem % 4u;
    if (pad) br.skip(pad);
    br.skip(br.remaining()); // defensive: consume rest
    
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      // length = 0 (bytes 4..7 already set by sendReply32, but make it explicit)
      wire::wr32_le(rep.data() + 4, 0);
      rep[1]  = 0; // present
      rep[8]  = 0; // major_opcode
      rep[9]  = 0; // first_event
      rep[10] = 0; // first_error
    });
  }
  
  // ---- 99: ListExtensions ----
  //
  // Reply:
  //   BYTE nExtensions (rep[1])
  //   length = 0, and no payload list.
  //
  // Bring-up: no extensions.
  void QueryOps::handleListExtensions(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    br.skip(br.remaining()); // request has no extra fields we care about
    
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      wire::wr32_le(rep.data() + 4, 0); // length=0
      rep[1] = 0; // nExtensions
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
    
    const uint8_t kSymsPerCode = 2;
    const uint32_t nSyms = (uint32_t)count * (uint32_t)kSymsPerCode;
    const uint32_t payloadBytes = nSyms * 4u;
    const uint32_t payloadWords = payloadBytes / 4u;
    
    std::vector<uint8_t> payload(payloadBytes, 0);
    
    const KeySyms2* map = getUSMacKeymap2();
    
    for (uint32_t i = 0; i < count; i++) {
      const uint8_t kc = (uint8_t)(first + (uint8_t)i);
      const KeySyms2 ks = map[kc];
      
      uint8_t* out = payload.data() + i * 8u; // 2 * 4 bytes
      wire::wr32_le(out + 0, ks.lo);
      wire::wr32_le(out + 4, ks.hi);
    }
    
    const bool ok = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = kSymsPerCode;
      wire::wr32_le(rep.data() + 4, payloadWords);
    });
    if (!ok) return;
    
    if (!payload.empty()) {
      (void)ctx.reply().sendBytes(payload.data(), payload.size());
    }
  }  

} // namespace x11
