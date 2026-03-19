//
//  HostCommandQueue.hpp
//  SwiftX11
//
//  Thread-safe queue for host commands (Cocoa/Swift thread → xproto thread).
//  Carries input events, resize notifications, surface updates, etc.
//

#pragma once
#include <cstdint>
#include <mutex>
#include <deque>

namespace x11 {

enum class HostCmdType : uint8_t {
  RootlessResize,
  SetPresentable,
  SurfaceResized,
  ExposeChildren,   // Force Expose to all mapped children (no BG fill)
  PointerMove,
  PointerEnter,
  PointerLeave,
  Button,
  ScrollTicks,
  Key,
  Focus,
  ClipboardCapture,  // proactive selection request: xid=owner, keyCode=selection atom
  WindowClose,       // user closed Cocoa window (red button or Cmd+W)
  ScreenLayoutChanged, // monitor hot-plug/unplug — send ConfigureNotify on root
  WindowMoved,         // user dragged NSWindow — send ConfigureNotify with new position
};

struct HostCmd {
  HostCmdType type;

  uint32_t xid = 0;

  // window resizing
  int32_t w_px = 0;
  int32_t h_px = 0;

  // pointer
  int32_t win_x_u = 0;
  int32_t win_y_u = 0;
  int32_t root_x_u = 0;
  int32_t root_y_u = 0;
  uint8_t deliver = 0;

  uint32_t buttonsMask = 0;
  uint32_t modsMask = 0;

  // buttons / scroll / keys
  uint8_t button = 0;
  uint8_t isDown = 0;
  int16_t ticks = 0;
  uint8_t axis = 0;
  uint32_t keyCode = 0;

  uint8_t focused = 0;
};

class HostCommandQueue {
public:
  void push(const HostCmd& c) {
    std::lock_guard<std::mutex> lock(mu_);

    // Coalesce consecutive RootlessResize for the same window.
    if (c.type == HostCmdType::RootlessResize && !q_.empty()) {
      HostCmd& back = q_.back();
      if (back.type == HostCmdType::RootlessResize && back.xid == c.xid) {
        back.w_px = c.w_px;
        back.h_px = c.h_px;
        return;
      }
    }

    // Coalesce consecutive PointerMove for the same window.
    // Prevents MotionNotify flood that live-locks Java AWT's XAWT thread
    // (holds AWT lock in XPending loop, starving EDT from clipboard ops).
    if (c.type == HostCmdType::PointerMove && !q_.empty()) {
      HostCmd& back = q_.back();
      if (back.type == HostCmdType::PointerMove && back.xid == c.xid) {
        if (back.deliver && !c.deliver) {
          // Pending event is inside-view (deliver=1), new event is outside-view
          // (deliver=0, from acceptsMouseMovedEvents).  Keep inside window coords
          // and deliver=1 so MotionNotify reaches the client.  Only update root
          // coords (for QueryPointer / xeyes global tracking).
          back.root_x_u   = c.root_x_u;
          back.root_y_u   = c.root_y_u;
        } else {
          back.win_x_u    = c.win_x_u;
          back.win_y_u    = c.win_y_u;
          back.root_x_u   = c.root_x_u;
          back.root_y_u   = c.root_y_u;
          back.deliver     = c.deliver;
          back.buttonsMask = c.buttonsMask;
          back.modsMask    = c.modsMask;
        }
        return;
      }
    }

    q_.push_back(c);
  }

  std::deque<HostCmd> takeAll() {
    std::lock_guard<std::mutex> lock(mu_);
    std::deque<HostCmd> out;
    out.swap(q_);
    return out;
  }

private:
  std::mutex mu_;
  std::deque<HostCmd> q_;
};

} // namespace x11
