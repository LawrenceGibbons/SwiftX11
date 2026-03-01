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
