//
//  EventOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once

#include <cstdint>
#include <cstddef>
#include <array>

#include <Transport/XProtoTransport.hpp>
#include <Core/XProtoContext.hpp>

namespace x11 {
  
  struct ConfigureNotifyParams {
    uint16_t seq = 0;
    uint32_t window = 0;
    int16_t x = 0, y = 0;
    uint16_t w = 0, h = 0;
    uint16_t borderWidth = 0;
    uint32_t aboveSibling = 0;
    bool overrideRedirect = false;
  };

  class XProtoContext;
  class ByteReader;
  
  // Central place for *server-generated* core protocol events.
  // (Expose, ConfigureNotify, MotionNotify, etc.)
  //
  // Design goals:
  //  - Event builders ONLY construct 32-byte wire events (no socket I/O here)
  //  - Queue helpers enqueue requests for the xproto/transport thread to flush
  //  - Transport can call flushPendingNotify(...) to build+send the right events
  //
  // Later you can route:
  //  - core events generated in response to requests (Expose, ConfigureNotify)
  //  - async input events (mouse/key) from your Swift side
  //  - extension events (XI2, RandR, XKB, Composite) by delegating into ExtEventOps.
  class EventOps {
  public:
    explicit EventOps(XProtoContext& ctx) : ctx_(ctx) {}
    
    // Dispatcher entry point for “event-related” requests if/when you add any.
    // For now: mostly unused; keeps the pattern consistent.
    void handle(uint8_t majorOpcode, uint8_t minorOpcode, ByteReader& br);
    
    // ---- Core event builders (32-byte X11 wire events) ----
    // These *only build the bytes*, they do NOT send on a socket.
//    static std::array<uint8_t, 32> buildExpose(uint16_t seq,
//                                               uint32_t window,
//                                               uint16_t x, uint16_t y,
//                                               uint16_t w, uint16_t h,
//                                               uint16_t count);
//    
    static std::array<uint8_t, 32> buildConfigureNotify(const ConfigureNotifyParams& p);
    
    // ---- Enqueue helpers (hook into your transport thread’s event queue) ----
    // The idea: xproto thread will periodically flush queued events to the client socket.
    // These functions can be called from anywhere *if* ctx routes them safely.
    void queueExpose(uint32_t window,
                     uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                     uint16_t count);
    
    // Preferred: struct-based API
    void queueConfigureNotify(const ConfigureNotifyParams& p);

    // Convenience overload (kept for call sites that haven't migrated yet)
    void queueConfigureNotify(uint32_t window,
                              int16_t x, int16_t y,
                              uint16_t w, uint16_t h,
                              uint16_t borderWidth,
                              uint32_t aboveSibling = 0,
                              bool overrideRedirect = false);
    
    // ---- Transport hook ----
    // Transport calls this during its flush pass (on the xproto thread) to turn a
    // coalesced PendingNotify into the appropriate wire events.
    //
    // seq is the sequence number Transport decided to stamp on the event.
    void flushPendingNotify(const PendingNotify& pn, uint16_t seq);
        
    // mouse handling
    void sendMotionNotify(XProtoContext& ctx,
                          uint32_t wid,
                          int32_t root_x, int32_t root_y,
                          uint32_t buttons, uint32_t mods);
    
    // mouse buttons
    void sendButtonEvent(XProtoContext& ctx,
                                   uint32_t wid,
                                   bool is_press,
                                   uint8_t button,
                                   int32_t root_x, int32_t root_y,
                                   uint32_t buttons, uint32_t mods,
                                   uint32_t child_xid /* = 0 */);
    // keyboard events
    void sendKeyEvent(XProtoContext& ctx,
                      uint32_t wid,
                      bool is_press,
                      uint8_t keycode,
                      uint32_t buttons, uint32_t mods);
    
    
    void sendCrossingEvent(XProtoContext& ctx,
                           uint32_t wid,
                           bool is_enter,
                           int32_t root_x, int32_t root_y,
                           uint32_t buttons, uint32_t mods,
                           uint8_t mode = 0);  // 0=Normal, 1=Grab, 2=Ungrab
    
    
    void sendFocusEvent(XProtoContext& ctx, uint32_t wid, bool is_in);

    // WM-initiated focus: bypasses FocusChangeMask check (matches SetInputFocus
    // behaviour — the focus target always receives the event).
    void sendFocusEventDirect(XProtoContext& ctx, uint32_t wid, bool is_in);

    // ---- XI2 (XInput2) GenericEvent senders ----
    // Each checks xi2_mask on the target window; no-op if the appropriate bit is not set.
    //
    // Return value: TRUE when the event was delivered via the window's OWN xi2_mask
    // (a per-window XISelectEvents selection by that window's client).  Callers use
    // this to suppress the matching CORE event, mirroring xorg's DeliverDeviceEvents
    // (dix/events.c): XI2 is tried first and, once it delivers, the walk breaks and
    // the core event is never sent — so a client selecting XI2 does not also receive
    // the core copy of the same physical event (which double-processed clicks in
    // Chromium/GTK).  A delivery that happened ONLY via the root-selection union
    // (xi2_root_mask) returns FALSE (it is not this window's own selection and
    // must not suppress core delivery to the window's own client).
    //
    // Crossing events are the EXCEPTION: xorg's DoEnterLeaveEvents
    // (dix/enterleave.c:595-608) sends the core AND the XI2 crossing
    // unconditionally, each gated only by its own mask.  Callers must not use
    // sendXI2CrossingEvent's return value to suppress the core EnterNotify/
    // LeaveNotify (v1.20.0.10 did; reverted in v1.20.0.12 — R1 in
    // docs/XI2_XORG_COMPARISON.md).
    bool sendXI2MotionEvent(XProtoContext& ctx, uint32_t wid,
                            int32_t root_x, int32_t root_y,
                            uint32_t buttons, uint32_t mods);

    bool sendXI2ButtonEvent(XProtoContext& ctx, uint32_t wid,
                            bool is_press, uint8_t button,
                            int32_t root_x, int32_t root_y,
                            uint32_t buttons, uint32_t mods,
                            uint32_t child_xid);

    bool sendXI2KeyEvent(XProtoContext& ctx, uint32_t wid,
                         bool is_press, uint8_t keycode,
                         uint32_t buttons, uint32_t mods);

    bool sendXI2CrossingEvent(XProtoContext& ctx, uint32_t wid,
                              bool is_enter,
                              int32_t root_x, int32_t root_y,
                              uint32_t buttons, uint32_t mods,
                              uint8_t mode = 0);  // 0=Normal, 1=Grab, 2=Ungrab

    void sendXI2FocusEvent(XProtoContext& ctx, uint32_t wid, bool is_in);

    // XI2 RawMotion: gated by the per-client root-selection union
    // (InputState::xi2_root_mask); delivered to the owner of `wid`.  xorg
    // fans raw events out to every root selector — that is the M5 follow-up.
    void sendXI2RawMotionEvent(XProtoContext& ctx, uint32_t wid);

  private:
    XProtoContext& ctx_;
  };
  
} // namespace x11
