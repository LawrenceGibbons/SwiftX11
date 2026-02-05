//
//  EventOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstddef>
#include <cstdint>

#include "EventOps.hpp"
#include "XProtoTransport.hpp"
#include "XProtoContext.hpp"
#include "ByteReader.hpp"
#include "ReplyWriter.hpp"
#include "WireLE.hpp"

namespace x11 {
  
//  // Little-endian writers (keep local for now; later share in a common helpers header).
//  static inline void wr16_le(uint8_t* p, uint16_t v) {
//    p[0] = (uint8_t)(v & 0xFF);
//    p[1] = (uint8_t)((v >> 8) & 0xFF);
//  }
//  static inline void wr32_le(uint8_t* p, uint32_t v) {
//    p[0] = (uint8_t)(v & 0xFF);
//    p[1] = (uint8_t)((v >> 8) & 0xFF);
//    p[2] = (uint8_t)((v >> 16) & 0xFF);
//    p[3] = (uint8_t)((v >> 24) & 0xFF);
//  }
  
  void EventOps::handle(uint8_t majorOpcode, uint8_t minorOpcode, ByteReader& br) {
    // For now, no core “event opcodes” exist in core X11.
    // This is here for symmetry + future growth.
    ctx_.tracef("[EventOps] handle major=%u minor=%u (stub, skipping %zu)\n",
                (unsigned)majorOpcode, (unsigned)minorOpcode, br.remaining());
    br.skip(br.remaining());
  }
  
  std::array<uint8_t, 32> EventOps::buildExpose(uint16_t seq,
                                                uint32_t window,
                                                uint16_t x, uint16_t y,
                                                uint16_t w, uint16_t h,
                                                uint16_t count) {
    std::array<uint8_t, 32> ev{};
    ev.fill(0);
    
    // Expose event type = 12
    ev[0] = 12;
    // ev[1] unused
    wire::wr16_le(ev.data() + 2, seq);
    wire::wr32_le(ev.data() + 4, window);
    wire::wr16_le(ev.data() + 8, x);
    wire::wr16_le(ev.data() + 10, y);
    wire::wr16_le(ev.data() + 12, w);
    wire::wr16_le(ev.data() + 14, h);
    wire::wr16_le(ev.data() + 16, count);
    return ev;
  }
  
  std::array<uint8_t, 32> EventOps::buildConfigureNotify(const ConfigureNotifyParams& p)
  {
    std::array<uint8_t, 32> ev{};
    ev.fill(0);
    
    // ConfigureNotify event type = 22
    ev[0] = 22;
    ev[1] = 0; // not synthetic
    wire::wr16_le(ev.data() + 2, p.seq);
    
    // event + window both set to window for a normal ConfigureNotify
    wire::wr32_le(ev.data() + 4, p.window); // event
    wire::wr32_le(ev.data() + 8, p.window); // window
    wire::wr32_le(ev.data() + 12, p.aboveSibling); // aboveSibling or None(0)
    
    // x/y are INT16 on the wire
    wire::wr16_le(ev.data() + 16, static_cast<uint16_t>(p.x));
    wire::wr16_le(ev.data() + 18, static_cast<uint16_t>(p.y));
    wire::wr16_le(ev.data() + 20, p.w);
    wire::wr16_le(ev.data() + 22, p.h);
    wire::wr16_le(ev.data() + 24, p.borderWidth);
    ev[26] = p.overrideRedirect ? 1 : 0;
    
    return ev;
  }
  
  void EventOps::queueExpose(uint32_t window,
                             uint16_t x, uint16_t y,
                             uint16_t w, uint16_t h,
                             uint16_t count)
  {
    // Enqueue a request for the xproto thread to emit an Expose for this window.
    // The Transport layer owns the actual fd and sequence tracking.
    ctx_.transport().queueNotify(window, /*wantConfigure*/false, /*wantExpose*/true);
    
    ctx_.tracef("[EventOps] queueExpose wid=0x%08X rect=(%u,%u %ux%u) count=%u\n",
                (unsigned)window,
                (unsigned)x, (unsigned)y,
                (unsigned)w, (unsigned)h,
                (unsigned)count);
    
    // NOTE: for now we ignore the rect in the queued notify and emit a full-window Expose
    // during flush (matches current bring-up behavior). We will carry rect later.
    (void)x; (void)y; (void)w; (void)h; (void)count;
  }
  
  void EventOps::queueConfigureNotify(const ConfigureNotifyParams& p)
  {
    // For now we only queue the fact that a ConfigureNotify should be emitted.
    // The transport flush pass will build the actual wire event using the latest
    // window geometry from XProtoContext.
    ctx_.transport().queueNotify(p.window, /*wantConfigure*/true, /*wantExpose*/false);
    
    ctx_.tracef("[EventOps] queueConfigureNotify wid=0x%08X xy=(%d,%d) wh=(%u,%u)\n",
                (unsigned)p.window,
                (int)p.x, (int)p.y,
                (unsigned)p.w, (unsigned)p.h);
    
    // NOTE: We currently do not carry the full ConfigureNotify payload through the pending queue.
    // Transport will emit the event using current window geometry at flush time.
    (void)p.borderWidth;
    (void)p.aboveSibling;
    (void)p.overrideRedirect;
    (void)p.seq;
  }
  
  void EventOps::queueConfigureNotify(uint32_t window,
                                      int16_t x, int16_t y,
                                      uint16_t w, uint16_t h,
                                      uint16_t borderWidth,
                                      uint32_t aboveSibling,
                                      bool overrideRedirect)
  {
    ConfigureNotifyParams p;
    p.window = window;
    p.x = x;
    p.y = y;
    p.w = w;
    p.h = h;
    p.borderWidth = borderWidth;
    p.aboveSibling = aboveSibling;
    p.overrideRedirect = overrideRedirect;
    queueConfigureNotify(p);
  }
  
  void EventOps::flushPendingNotify(const PendingNotify& pn, uint16_t seq) {
    if (pn.wid == 0) return;
    
    const WindowView* w = ctx_.window(pn.wid);
    if (!w) return;
    
    // Only send what the client asked for (mask check mirrors C).
    if (pn.want_configure) {
      if ((w->event_mask & (1u << 17)) && w->owner_fd > 0) {
        ConfigureNotifyParams p;
        p.seq = seq;
        p.window = pn.wid;
        p.x = w->x;
        p.y = w->y;
        p.w = static_cast<uint16_t>(w->w);
        p.h = static_cast<uint16_t>(w->h);
        p.borderWidth = 0;
        p.aboveSibling = 0;
        p.overrideRedirect = false;
        auto ev = buildConfigureNotify(p);
        ctx_.transport().sendEvent32(pn.wid, ev.data());
      }
    }
    
    if (pn.want_expose) {
      if (w->mapped && (w->event_mask & (1u << 15)) && w->owner_fd > 0) {
        auto ev = buildExpose(seq, pn.wid, 0, 0, w->w, w->h, 0);
        ctx_.transport().sendEvent32(pn.wid, ev.data());
      }
    }
  }
  
  void EventOps::sendMotionNotify(XProtoContext& ctx,
                                  uint32_t wid,
                                  int32_t root_x, int32_t root_y,
                                  uint32_t buttons, uint32_t mods)
  {
    // Clamp to 16-bit signed coordinate range used by core events
    auto clamp16 = [](int32_t v) -> int16_t {
      if (v < -32768) return -32768;
      if (v >  32767) return  32767;
      return (int16_t)v;
    };

    const int16_t rx = clamp16(root_x);
    const int16_t ry = clamp16(root_y);

    uint8_t ev[32] = {0};

    ev[0] = 6;   // MotionNotify
    ev[1] = 0;   // detail

    // Sequence: if you have a stored "last seq" in transport, use it. Otherwise 0 is acceptable.
    // If you *do* have a method, replace 0 with that.
    wire::wr16_le(ev + 2, 0);

    // Time (ms). 0 is acceptable for bring-up.
    wire::wr32_le(ev + 4, 0);

    // Root window XID (you advertise root=1 in SetupSuccess)
    wire::wr32_le(ev + 8, 1);

    // Event window
    wire::wr32_le(ev + 12, wid);

    // Child (none for now)
    wire::wr32_le(ev + 16, 0);

    // rootX/rootY
    wire::wr16_le(ev + 20, (uint16_t)rx);
    wire::wr16_le(ev + 22, (uint16_t)ry);

    // eventX/eventY
    // For now, treat same as root coords. (Enough for xeyes; later subtract window origin.)
    wire::wr16_le(ev + 24, (uint16_t)rx);
    wire::wr16_le(ev + 26, (uint16_t)ry);

    // state: combine button/modifier masks from Swift (you already encode these)
    wire::wr16_le(ev + 28, (uint16_t)(buttons | mods));

    ev[30] = 1;  // sameScreen
    ev[31] = 0;

    ctx.transport().sendEvent32(wid, ev);
  }
} // namespace x11

