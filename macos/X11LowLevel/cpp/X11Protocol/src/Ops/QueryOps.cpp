//
//  QueryOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstdio>

#include "QueryOps.hpp"
#include "XProtoContext.hpp"
#include "ReplyWriter.hpp"
#include "ByteReader.hpp"

// C bridge for hierarchy queries (implemented in x11_xproto.c)
#include "XProtoQueryBridge.h"

namespace x11 {
    
  static constexpr uint32_t kRootXid = 0x00000001u;
  static constexpr uint16_t kRootW   = 800;
  static constexpr uint16_t kRootH   = 600;
  static constexpr uint16_t kDepth   = 24;
  
  QueryOps::QueryOps(XProtoRegistrar& reg) {
    reg.registerMajor(14, &QueryOps::onMajor, this); // GetGeometry
    reg.registerMajor(15, &QueryOps::onMajor, this); // QueryTree
    reg.registerMajor(38, &QueryOps::onMajor, this); // QueryPointer
    reg.registerMajor(43, &QueryOps::onMajor, this); // GetInputFocus
  }

  void QueryOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
    if (!user) { dc.br.skip(dc.br.remaining()); return; }
    static_cast<QueryOps*>(user)->handle(ctx, dc);
  }

  void QueryOps::handle(XProtoContext& ctx, DispatchContext& dc) {
    switch (dc.major) {
      case 14: handleGetGeometry(ctx, dc.seq, dc.br); return;
      case 15: handleQueryTree(ctx, dc.seq, dc.br); return;
      case 38: handleQueryPointer(ctx, dc.seq, dc.br); return;
      case 43: handleGetInputFocus(ctx, dc.seq, dc.br); return;
      default: 
        dc.br.skip(dc.br.remaining());
        ctx.tracef("[QueryOps] unexpected major=%u\n", (unsigned)dc.major);
        return;
    }
  }
  
  // ---- 14: GetGeometry ----
  void QueryOps::handleGetGeometry(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    // Request: CARD32 drawable
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t drawable = br.readU32();
    br.skip(br.remaining());
    
    uint32_t root = kRootXid;
    int16_t  x = 0, y = 0;
    uint16_t w = kRootW, h = kRootH;
    uint16_t border = 0;
    
    // If drawable is a known window, return its geometry.
    if (const WindowView* vw = ctx.window(drawable)) {
      x = vw->x;
      y = vw->y;
      w = vw->w;
      h = vw->h;
    }
    
    // Use ReplyWriter helper (already in your code)
    (void)ctx.reply().sendGetGeometryReply(seq, root, x, y, w, h, border, kDepth);
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

    // Fake pointer motion (same as old C behavior)
    fake_rx_ = (int16_t)((fake_rx_ + 1) % 200);
    fake_ry_ = (int16_t)((fake_ry_ + 1) % 120);

    uint32_t child = 0;
    int16_t  winx = 0, winy = 0;

    if (const WindowView* w = ctx.window(qwin)) {
      if (w->mapped) {
        child = qwin;

        int32_t tx = (int32_t)fake_rx_ - (int32_t)w->x;
        int32_t ty = (int32_t)fake_ry_ - (int32_t)w->y;

        if (tx < 0) tx = 0;
        if (ty < 0) ty = 0;
        if (tx > (int32_t)w->w) tx = (int32_t)w->w;
        if (ty > (int32_t)w->h) ty = (int32_t)w->h;

        winx = (int16_t)tx;
        winy = (int16_t)ty;
      }
    }

    // Build the fixed 32-byte reply using ReplyWriter.
    // NOTE: QueryPointer reply fields are:
    //   rep[1]  = sameScreen (bool)
    //   8..11   = root (WINDOW)
    //   12..15  = child (WINDOW)
    //   16..17  = rootX (INT16)
    //   18..19  = rootY (INT16)
    //   20..21  = winX  (INT16)
    //   22..23  = winY  (INT16)
    //   24..25  = mask  (CARD16)
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      rep[1] = 1; // sameScreen = true
      ReplyWriter::wr32_le(rep.data() + 8,  kRootXid);
      ReplyWriter::wr32_le(rep.data() + 12, child);
      ReplyWriter::wr16_le(rep.data() + 16, (uint16_t)fake_rx_);
      ReplyWriter::wr16_le(rep.data() + 18, (uint16_t)fake_ry_);
      ReplyWriter::wr16_le(rep.data() + 20, (uint16_t)winx);
      ReplyWriter::wr16_le(rep.data() + 22, (uint16_t)winy);
      ReplyWriter::wr16_le(rep.data() + 24, 0);
    });
  }

  
  // ---- 15: QueryTree ----
  void QueryOps::handleQueryTree(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
    if (br.remaining() < 4) { br.skip(br.remaining()); return; }
    const uint32_t wid = br.readU32();
    br.skip(br.remaining());
    
    uint32_t parent = 0;
    uint32_t children[256];
    uint32_t nchildren = 0;
    
    // Ask C side to compute parent + children from the authoritative g_wins[]
    const int ok = x11_xproto_query_tree(wid, &parent, children, 256, &nchildren);
    
    if (!ok) {
      parent = 0;
      nchildren = 0;
    }
    
    if (nchildren > 256) nchildren = 256;
    const uint32_t extra_words = nchildren; // each child is CARD32
    
    // Build + send 32-byte reply header
    const bool okHdr = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
      // length_words at bytes 4..7
      ReplyWriter::wr32_le(rep.data() + 4, extra_words);
      
      // root at 8..11
      ReplyWriter::wr32_le(rep.data() + 8, kRootXid);
      
      // parent at 12..15
      ReplyWriter::wr32_le(rep.data() + 12, parent);
      
      // nchildren at 16..17 (CARD16)
      ReplyWriter::wr16_le(rep.data() + 16, (uint16_t)nchildren);
    });
    
    if (!okHdr) return;
    
    // Follow with children list (CARD32[]), already 4-byte aligned
    if (nchildren) {
      uint8_t out[256 * 4];
      for (uint32_t i = 0; i < nchildren; i++) {
        ReplyWriter::wr32_le(out + (size_t)i * 4u, children[i]);
      }
      (void)ctx.transport().sendReplyBytes(out, (std::size_t)nchildren * 4u);
    }
  } 
} // namespace x11
