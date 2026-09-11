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
#include "Core/InputRouting.hpp"    // pickDeepestMappedWindowAtHostPoint (QueryPointer child)
#include "Core/GrabTable.hpp"        // keyboard grab → NotifyWhileGrabbed (SetInputFocus)
#include "Core/XProtoServer.hpp"     // eventOps() for the focus choreography
#include "Utils/FocusEvents.hpp"     // Phase D: DoFocusEvents
#include "XProtoNotifyBridge.hpp"    // postMotion — WarpPointer event synthesis (C7)

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
    if (qwin != kRootWindowXid && qwin != 0) {
      WindowView tmp{};
      if (!ctx.windows().snapshot(qwin, tmp)) {
        ctx.transport().sendErrorCore(x11::error::BadWindow, seq, qwin, x11::opcode::QueryPointer);
        return;
      }
    }

    const auto& in = ctx.input();

    // xorg ProcQueryPointer (dix/events.c): root = the sprite's root position;
    // one screen, so sameScreen is always True and winX/winY are the sprite
    // position relative to the queried window's root origin wherever the
    // pointer is; `child` is the direct child of the queried window on the
    // sprite path, None otherwise.  The old answer used the last motion
    // host's local coords and a cached host origin, so a window the pointer
    // had never entered got (0,0) — xeyes polling its own window looked at
    // its top-left corner for ever (Phase E pass, v1.20.0.33).
    const int32_t rootx32 = in.root_x_u;
    const int32_t rooty32 = in.root_y_u;
    // Wire-format state: internal button bits 0-4 must map to X11
    // positions 8-12 (raw buttons|mods reported Button1 as ShiftMask —
    // Java drag loops polling XQueryPointer saw "no buttons held").
    const uint16_t mask = x11::input::toX11State(in.buttons, in.mods);
    auto clamp16 = [](int32_t v) -> int16_t {
      if (v < -32768) return -32768;
      if (v >  32767) return  32767;
      return (int16_t)v;
    };
    // Host the pointer was last seen in (0 = none); its cached local coords
    // follow the real position through the tracker (M8), so the pick fails
    // cleanly once the pointer has left the host.
    const uint32_t host = in.last_xid;
    const uint32_t spriteWin = host ? pickDeepestMappedWindowAtHostPoint(ctx, host, in.win_x_u, in.win_y_u) : 0;

    uint32_t child = 0;
    int32_t winx32 = rootx32, winy32 = rooty32;
    if (qwin != kRootWindowXid && qwin != 0) {
      int32_t ox = 0, oy = 0;
      uint32_t cur = qwin;
      for (int hop = 0; hop < 256 && cur && cur != kRootWindowXid; hop++) {
        WindowView cv{};
        if (!ctx.windows().snapshot(cur, cv)) break;
        ox += (int32_t)cv.x + (int32_t)cv.border_width;
        oy += (int32_t)cv.y + (int32_t)cv.border_width;
        cur = cv.parent_xid;
      }
      winx32 = rootx32 - ox;
      winy32 = rooty32 - oy;
      if (spriteWin && ctx.windows().topLevelAncestorOf(qwin) == host) {
        uint32_t t = spriteWin;
        for (int hop = 0; hop < 64 && t && t != kRootWindowXid; hop++) {
          WindowView tv{};
          if (!ctx.windows().snapshot(t, tv)) break;
          if (tv.parent_xid == qwin) { child = t; break; }
          t = tv.parent_xid;
        }
      }
    } else {
      child = spriteWin ? host : 0;   // root query: the toplevel under the pointer
    }

    const int16_t rootx = clamp16(rootx32), rooty = clamp16(rooty32);
    const int16_t winx = clamp16(winx32), winy = clamp16(winy32);
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = 1; // sameScreen
      wire::wr32_le(rep.data() + 8,  kRootWindowXid);
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
    
    uint32_t parent = 0;
    uint32_t children[256];
    uint32_t nchildren = 0;
    
    // Use authoritative WindowTable (no C bridge)
    bool ok = ctx.windows().queryTree(wid, &parent, children, 256, &nchildren);
    if (!ok) {
      // Root window (XID 1) is always valid even though it's not in WindowTable
      if (wid != kRootWindowXid && wid != 0) {
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
      wire::wr32_le(rep.data() + 8, kRootWindowXid);      // root
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
      // R3 F3: real error base so GDK/Cairo error traps can classify
      // BadPicture/BadPictFormat/BadGlyphSet/BadGlyph (was 0 = "no error").
      first_error = ext::kRENDER_FirstError;
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

    // Maximum request length: match xorg's MAX_BIG_REQUEST_SIZE (os.h) so a
    // legitimately-large request (Java2D can PutImage well over 4MB) fits
    // instead of tripping our old 4MB ceiling and killing the connection
    // (2026-09-08 review B3).  4194303 words = 16 MB − 4 B.
    static constexpr uint32_t kMaxBigReqWords = 0x003FFFFFu; // 4194303 words

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
  if (srcWin != kRootWindowXid && srcWin != 0) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(srcWin, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, srcWin, x11::opcode::TranslateCoords);
      return;
    }
  }
  // Validate dstWin exists (root is always valid)
  if (dstWin != kRootWindowXid && dstWin != 0) {
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
    for (int hop = 0; hop < 256 && cur && cur != kRootWindowXid; hop++) {
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
  if (dstWin != kRootWindowXid) {
    uint32_t cur = dstWin;
    for (int hop = 0; hop < 256 && cur && cur != kRootWindowXid; hop++) {
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

// A window's origin in X11 root coordinates.  A top-level's WindowView.x/y are
// already root coords; a child adds its offset within its host.  Root/None
// resolve to (0,0).
static void windowRootOrigin(XProtoContext& ctx, uint32_t win, int32_t& outX, int32_t& outY) {
  outX = 0; outY = 0;
  if (win == 0 || win == kRootWindowXid) return;
  uint32_t host = ctx.windows().topLevelAncestorOf(win);
  if (host == 0) host = win;
  WindowView hv{};
  if (!ctx.windows().snapshot(host, hv)) return;
  int32_t ox = 0, oy = 0;
  if (win != host) ctx.windows().absoluteOffsetInHost(host, win, ox, oy);
  outX = (int32_t)hv.x + ox;
  outY = (int32_t)hv.y + oy;
}

// The top-level host whose root-space footprint contains (rx, ry), or 0 for
// none.  Iterates root's children topmost-last (our stacking order; Cocoa owns
// normal-window Z, so overlapping normals are approximate — the common warp
// lands over a single window).
static uint32_t hostContainingRootPoint(XProtoContext& ctx, int32_t rx, int32_t ry) {
  uint32_t found = 0;
  for (uint32_t top : ctx.windows().childrenInStackOrder(kRootWindowXid)) {
    WindowView vw{};
    if (!ctx.windows().snapshot(top, vw) || !vw.mapped) continue;
    const int32_t bw = (int32_t)vw.border_width;
    if (rx >= (int32_t)vw.x - bw && rx < (int32_t)vw.x + (int32_t)vw.w + bw &&
        ry >= (int32_t)vw.y - bw && ry < (int32_t)vw.y + (int32_t)vw.h + bw)
      found = top;   // keep the last (topmost) match
  }
  return found;
}

// ---- 41: WarpPointer ----
// Body (20 bytes): srcWindow(4, 0=None), dstWindow(4, 0=None),
//                  srcX(2), srcY(2), srcWidth(2), srcHeight(2), dstX(2), dstY(2).
void QueryOps::handleWarpPointer(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t srcWin = br.readU32();
  const uint32_t dstWin = br.readU32();
  const int16_t  srcX   = br.readI16();
  const int16_t  srcY   = br.readI16();
  const uint16_t srcW   = br.readU16();
  const uint16_t srcH   = br.readU16();
  const int16_t  dstX   = br.readI16();
  const int16_t  dstY   = br.readI16();
  br.skip(br.remaining());

  // Validate src/dst windows — xorg ProcWarpPointer (dix/events.c:3697-3711):
  // dixLookupWindow on either → BadWindow.  None(0) and root are valid.
  if (srcWin != 0 && srcWin != kRootWindowXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(srcWin, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, srcWin, x11::opcode::WarpPointer);
      return;
    }
  }
  if (dstWin != 0 && dstWin != kRootWindowXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(dstWin, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, dstWin, x11::opcode::WarpPointer);
      return;
    }
  }

  const int32_t curRootX = ctx.input().root_x_u;
  const int32_t curRootY = ctx.input().root_y_u;

  // Src-window constraint — xorg dix/events.c:3705-3722: with srcWin != None
  // the pointer must lie within [srcX, srcX+srcWidth] x [srcY, srcY+srcHeight]
  // of srcWin (a 0 width/height is unbounded on that axis); otherwise the warp
  // is a silent no-op.  (PointInWindowIsVisible is not modelled — rootless.)
  if (srcWin != 0) {
    int32_t sox = 0, soy = 0;
    windowRootOrigin(ctx, srcWin, sox, soy);
    if (curRootX < sox + srcX || curRootY < soy + srcY ||
        (srcW != 0 && sox + srcX + (int32_t)srcW < curRootX) ||
        (srcH != 0 && soy + srcY + (int32_t)srcH < curRootY)) {
      return;
    }
  }

  // Destination in root coordinates.
  int32_t newRootX, newRootY;
  if (dstWin == 0) {                 // relative to the current pointer
    newRootX = curRootX + dstX;
    newRootY = curRootY + dstY;
  } else {                           // relative to dstWin's origin (root for kRootWindowXid)
    int32_t dox = 0, doy = 0;
    windowRootOrigin(ctx, dstWin, dox, doy);
    newRootX = dox + dstX;
    newRootY = doy + dstY;
  }

  // Move the OS cursor (Swift CGWarpMouseCursorPosition).  UI queue convention:
  //   xid == 0 → relative warp (deltas from the current OS cursor)
  //   xid != 0 → absolute, host-window-local coordinates
  if (dstWin == 0) {
    x11_ui_push_warp_pointer(0, (int32_t)dstX, (int32_t)dstY);
  } else {
    uint32_t target = (dstWin == kRootWindowXid) ? 0 : dstWin;
    uint32_t host = 0;
    int32_t offX = 0, offY = 0;
    if (target != 0) {
      host = ctx.windows().topLevelAncestorOf(target);
      if (host == 0) host = target;
      ctx.windows().absoluteOffsetInHost(host, target, offX, offY);
    }
    x11_ui_push_warp_pointer(host, (int32_t)dstX + offX, (int32_t)dstY + offY);
  }

  // Synthesize the motion the warp implies — xorg ProcWarpPointer →
  // SetCursorPosition(generateEvent=TRUE) → miPointerMove →
  // GetPointerEvents(MotionNotify) → CheckMotion (mi/mipointer.c:388,744;
  // dix/events.c:3757): a MotionNotify, the Enter/Leave when the sprite window
  // changes, and the XI2 RawMotion.  CGWarpMouseCursorPosition emits no OS
  // mouse-moved event and nothing else would synthesize one, so InputState
  // would stay stale and Java Robot / warp-based UIs would see a pointer that
  // "didn't move" (C7).  A zero-distance warp short-circuits like xorg's
  // miPointerMoveNoEvent.  Canonical button/mod state via the 0xFFFFFFFF
  // sentinel (postMotion substitutes InputState — M8).
  if (newRootX != curRootX || newRootY != curRootY) {
    const uint32_t landHost = hostContainingRootPoint(ctx, newRootX, newRootY);
    int32_t localX = newRootX, localY = newRootY;
    uint8_t deliver = 0;
    if (landHost) {
      WindowView hv{};
      if (ctx.windows().snapshot(landHost, hv)) {
        localX = newRootX - (int32_t)hv.x;
        localY = newRootY - (int32_t)hv.y;
        deliver = 1;
      }
    }
    x11::notify::postMotion(landHost, localX, localY, newRootX, newRootY, deliver,
                            /*buttons*/0xFFFFFFFFu, /*mods*/0xFFFFFFFFu);
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
  if (focus != 0 && focus != 1 && focus != kRootWindowXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(focus, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, focus, x11::opcode::SetInputFocus);
      return;
    }
  }

  const uint32_t oldFocus = ctx.input().focus_xid;
  const uint32_t newFocus = (focus == 0) ? 0 : (focus == 1) ? kPointerRootFocus : focus;

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
