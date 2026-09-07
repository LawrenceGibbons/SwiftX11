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
#include <set>

#include "Ops/QueryOps.hpp"
#include "Core/XClient.hpp"
#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/ByteReader.hpp"
#include "Core/XConstants.hpp"
#include "Utils/WireLE.hpp"
#include "Core/KeySyms.hpp"
#include "Core/CoreKeymap.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/X11ExtOpcodes.hpp"
#include "Core/X11Modifiers.hpp"
#include "Core/GrabTable.hpp"        // keyboard grab → NotifyWhileGrabbed (SetInputFocus)
#include "Core/XProtoServer.hpp"     // eventOps() for the focus choreography
#include "Utils/FocusEvents.hpp"     // Phase D: DoFocusEvents

extern "C" {
#include "SwiftX11Bridge.h"
#include "Utils/MachTime.hpp"
}
extern "C" x11::XProtoServer* x11_proto_bridge_get_server(void);

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
  // The keycode → keysym table lives in Core/CoreKeymap.cpp (v1.20.0.20) so
  // the XKEYBOARD keymap builder derives its key types and symbol maps from
  // the very same data GetKeyboardMapping serves.

  // MARK: - Handlers
  // ---- 43: GetInputFocus ----
  void QueryOps::handleGetInputFocus(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    br.skip(br.remaining());
    const uint32_t f = ctx.input().focus_xid ? ctx.input().focus_xid : 0;
    (void)ctx.reply().sendGetInputFocusReply(seq, ctx.input().focus_revert_to, f);
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
    // Wire-format state: internal button bits 0-4 must map to X11
    // positions 8-12 (raw buttons|mods reported Button1 as ShiftMask —
    // Java drag loops polling XQueryPointer saw "no buttons held").
    const uint16_t mask = x11::input::toX11State(in.buttons, in.mods);
    
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
      // GE allocates 0 events — first_event should be 0.
      // GenericEvent (type 35) is a core protocol type, not an extension event.
    } else if (name == "SHAPE") {
      present = 1; major = ext::kSHAPE;
      first_event = ext::kSHAPE_FirstEvent;
    } else if (name == "XC-MISC") {
      present = 1; major = ext::kXCMisc;
    } else if (name == "XInputExtension") {
      // Runtime toggle (Settings → "XInput2"), default ON since v1.20.0.22 after
      // Phases A–B (delivery, grabs) and F (XKEYBOARD, which GTK3 needs for keys
      // over XI2) were verified against xterm, Vivado, Vitis and portal-GTK.
      // OFF keeps Electron/GTK clients on the core input path if something
      // misbehaves.  first_event MUST be >= 64: libXi registers 17 wire-to-event
      // handlers from here; 0 would clobber core handlers.
      if (x11_get_xi2_advertised()) {
        present = 1; major = ext::kXInput2;
        first_event = ext::kXInput_FirstEvent;
        // BadDevice = first_error + 0 (Xi/extinit.c:1065); with 0 every device
        // error was code 0, which GDK's error traps read as "no error" (M13).
        first_error = ext::kXInput_FirstError;
      } else {
        present = 0;
      }
    } else if (name == "XTEST") {
      present = 1; major = ext::kXTEST;
    } else if (name == "Composite") {
      present = 1; major = ext::kCOMPOSITE;
    } else if (name == "XKEYBOARD") {
      // Runtime toggle (Settings → "XKEYBOARD"), default ON since v1.20.0.22
      // (M23 / Phase F, verified 2026-09-07).  Once present, libX11/GDK/AWT/
      // Chromium all translate keycodes through XkbGetMap with no fallback to
      // the core tables, so the whole client-info surface must stay served
      // (Extensions/XKBOps.cpp).  One event code, one error code.
      if (x11_get_xkb_advertised()) {
        present = 1; major = ext::kXKB;
        first_event = ext::kXKB_FirstEvent;
        first_error = ext::kXKB_FirstError;
      } else {
        present = 0;
      }
    }
    // DAMAGE deliberately NOT advertised (M4): we never generate DamageNotify
    // — a rootless server has no compositor to feed, so every DamageCreate
    // would silently never fire.  Advertising it was dishonest.  The major
    // handler stays dormant; without a QueryExtension major, no client reaches
    // it.  The EXT_PROBE log below records anyone who asks, so we can gauge
    // real-world demand for it (or any other extension) within rootless limits.

#ifndef NDEBUG
    TS_FPRINTF("[QueryExtension] \"%s\" -> present=%u major=%u\n",
            name.c_str(), (unsigned)present, (unsigned)major);
#endif

    // Surface outside demand: log the first time each *unadvertised* extension
    // is requested this session.  The xproto dispatch is single-threaded, so a
    // function-local static set needs no lock.  Goes to the SwiftX11 console.
    if (present == 0 && !name.empty()) {
      static std::set<std::string> s_probed;
      if (s_probed.insert(name).second) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "[EXT_PROBE] client requested unadvertised extension \"%s\" "
                 "(not supported — rootless server)\n", name.c_str());
        x11_ui_push_log(1, buf);
      }
    }

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
    // XInputExtension is appended only when advertised (runtime toggle) — kept
    // in sync with the QueryExtension handler's present flag.
    static const char* base_extensions[] = {
      "BIG-REQUESTS",
      "RENDER",
      "XFIXES",
      "RANDR",
      "XINERAMA",
      "Generic Event Extension",
      "SHAPE",
      "XC-MISC",
      "XTEST",
      "Composite",
      // DAMAGE removed (M4): advertised-but-silent; see handleQueryExtension.
    };
    std::vector<const char*> extensions(base_extensions,
                                        base_extensions + (sizeof(base_extensions) / sizeof(base_extensions[0])));
    if (x11_get_xi2_advertised()) extensions.push_back("XInputExtension");
    if (x11_get_xkb_advertised()) extensions.push_back("XKEYBOARD");
    const uint8_t nExt = (uint8_t)extensions.size();

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

    const uint32_t nSyms = (uint32_t)count * (uint32_t)kCoreKeysymsPerKeycode;
    const uint32_t payloadBytes = nSyms * 4u;
    const uint32_t payloadWords = payloadBytes / 4u;

    std::vector<uint8_t> payload(payloadBytes, 0);

    // Shared with the XKEYBOARD keymap builder (Core/CoreKeymap.cpp).
    const KeySyms4* map = coreKeyboardMap();

    for (uint32_t i = 0; i < count; i++) {
      const uint8_t kc = (uint8_t)(first + (uint8_t)i);
      const KeySyms4& ks = map[kc];

      uint8_t* out = payload.data() + i * (kCoreKeysymsPerKeycode * 4u);
      for (uint8_t col = 0; col < kCoreKeysymsPerKeycode; col++) {
        wire::wr32_le(out + col * 4u, ks.syms[col]);
      }
    }

    const bool ok = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = kCoreKeysymsPerKeycode;
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

  // Spec: `child` = the mapped direct child of dst containing the point,
  // topmost wins (children_order_ is bottom→top, so last match wins).
  // Was hardcoded to None — AWT's XDnD descends the hierarchy through
  // this field, so drop-target discovery terminated at the toplevel
  // (review 2026-08-31 §2.10).
  uint32_t childWin = 0;
  for (uint32_t c : ctx.windows().childrenInStackOrder(dstWin)) {
    WindowView cv{};
    if (!ctx.windows().snapshot(c, cv) || !cv.mapped) continue;
    const int32_t cx0 = (int32_t)cv.x;
    const int32_t cy0 = (int32_t)cv.y;
    const int32_t cx1 = cx0 + (int32_t)cv.w + 2 * (int32_t)cv.border_width;
    const int32_t cy1 = cy0 + (int32_t)cv.h + 2 * (int32_t)cv.border_width;
    if (dstX >= cx0 && dstX < cx1 && dstY >= cy0 && dstY < cy1) {
      childWin = c;
    }
  }

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = 1; // sameScreen
    wire::wr32_le(rep.data() + 8, childWin);
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

  // xorg SetInputFocus (dix/events.c:4899-4990): DoFocusEvents(old, new,
  // NotifyWhileGrabbed under a keyboard grab, else NotifyNormal) — core and
  // XI2 FocusOut/FocusIn at every affected window with the detail derived
  // from the window relation (Phase D, M9; Utils/FocusEvents.hpp).
  {
    uint8_t mode = x11::notifymode::kNormal;
    KeyboardGrab kg{};
    if (ctx.grabs().getKeyboardGrabInfo(kg) && kg.active) mode = x11::notifymode::kWhileGrabbed;
    if (auto* srv = x11_proto_bridge_get_server())
      x11::focusev::doFocusEvents(ctx, srv->eventOps(), oldFocus, newFocus, mode);
  }

  // Update X11-level input focus — only focus_xid, NOT focus_host.
  // focus_host tracks the Cocoa key window and must only be set by
  // HostCmdType::Focus (didBecomeKey/didResignKey).  Updating it here
  // would desync from Cocoa and defeat the duplicate-focus guard,
  // enabling WM_TAKE_FOCUS bounce loops between dialog and main window.
  ctx.input().focus_xid = newFocus;

  // §2.9: remember revert-to so destroy/unmap of the focus window can
  // revert to Parent/PointerRoot instead of leaving the keyboard dead.
  ctx.input().focus_revert_to = (revertTo <= 2) ? revertTo : 0;
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
