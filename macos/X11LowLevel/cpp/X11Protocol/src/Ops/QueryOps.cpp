//
//  QueryOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstdio>
#include <vector>

#include "QueryOps.hpp"
#include "XProtoContext.hpp"
#include "WindowTable.hpp"
#include "ReplyWriter.hpp"
#include "ByteReader.hpp"
#include "XConstants.hpp"
#include "WireLE.hpp"


namespace x11 {
    
  QueryOps::QueryOps(XProtoRegistrar& reg) {
    reg.registerMajor(15, &QueryOps::onMajor, this); // QueryTree
    reg.registerMajor(38, &QueryOps::onMajor, this); // QueryPointer
    reg.registerMajor(43, &QueryOps::onMajor, this); // GetInputFocus
    reg.registerMajor(91, &QueryOps::onMajor, this); // QueryColors
    reg.registerMajor(98, &QueryOps::onMajor, this); // QueryExtension
    reg.registerMajor(99, &QueryOps::onMajor, this); // ListExtensions
  }

  void QueryOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
    if (!user) { dc.br.skip(dc.br.remaining()); return; }
    static_cast<QueryOps*>(user)->handle(ctx, dc);
  }

  void QueryOps::handle(XProtoContext& ctx, DispatchContext& dc) {
    switch (dc.major) {
      case 15: handleQueryTree(ctx, dc.seq, dc.br); return;
      case 38: handleQueryPointer(ctx, dc.seq, dc.br); return;
      case 43: handleGetInputFocus(ctx, dc.seq, dc.br); return;
      case 91: handleQueryColors(ctx, dc.seq, dc.br); return;
      case 98: handleQueryExtension(ctx, dc.seq, dc.br); return;
      case 99: handleListExtensions(ctx, dc.seq, dc.br); return;

      default: 
        dc.br.skip(dc.br.remaining());
        ctx.tracef("[QueryOps] unexpected major=%u\n", (unsigned)dc.major);
        return;
    }
  }
  

  
  // ---- 43: GetInputFocus ----
  void QueryOps::handleGetInputFocus(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    br.skip(br.remaining());
    // Bring-up behavior: focus = root, revertTo=None(0)
    (void)ctx.reply().sendGetInputFocusReply(seq, /*revertTo*/0, /*focus*/kRootXid);
  }
  
  // ---- 38: QueryPointer ----
  void QueryOps::handleQueryPointer(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t qwin = br.readU32();
    br.skip(br.remaining());

    const auto& in = ctx.input();

    // Root coords (global, top-left)
    const int32_t rootx32 = in.root_x;
    const int32_t rooty32 = in.root_y;
    const uint16_t mask = (uint16_t)(in.buttons | in.mods);

    // Host-local coords (relative to host_xid view)
    const uint32_t host = in.last_xid;
    const int32_t hostx = in.win_x;
    const int32_t hosty = in.win_y;

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
  // Bring-up behavior matches legacy:
  //   pixel==0 => black, else white.
  void QueryOps::handleQueryColors(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }

    const uint32_t cmap = br.readU32();
    (void)cmap; // bring-up: ignore colormap

    // Number of pixels is implied by remaining request length.
    uint16_t ncolors = (uint16_t)(br.remaining() / 4u);
    if (ncolors > 1024) ncolors = 1024;

    // Each xrgb = 8 bytes = 2 words.
    const uint32_t extra_words = (uint32_t)ncolors * 2u;

  #ifndef NDEBUG
    ctx.tracef("[QueryColors] seq=%u n=%u extra_words=%u req_remain=%zu\n",
               (unsigned)seq, (unsigned)ncolors, (unsigned)extra_words, br.remaining());
  #endif

    const bool ok = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      // length (words) at bytes 4..7
      wire::wr32_le(rep.data() + 4, extra_words);

      // nColors at bytes 8..9 (CARD16)
      wire::wr16_le(rep.data() + 8, ncolors);

      // byte 1 is "unused" for this reply; leaving as whatever sendReply32 sets (usually 0) is fine.
    });
    if (!ok) return;

    // Stream payload: ncolors entries, 8 bytes each (already 4-byte aligned).
    for (uint16_t i = 0; i < ncolors; i++) {
      const uint32_t pix = br.readU32();

      uint8_t out[8];
      if (pix == 0) {
        // black
        out[0] = out[1] = 0;
        out[2] = out[3] = 0;
        out[4] = out[5] = 0;
      } else {
        // white (0xFFFF in 16-bit)
        out[0] = out[1] = 0xFF;
        out[2] = out[3] = 0xFF;
        out[4] = out[5] = 0xFF;
      }
      out[6] = 0;
      out[7] = 0;

      (void)ctx.reply().sendPaddedBytes(out, sizeof(out)); // 8 bytes; no extra pad needed
    }

    // Defensive: consume any trailing bytes (if request length wasn't a multiple of 4)
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

  
  
} // namespace x11
