//
//  UICommandQueue.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/6/26.
//

#pragma once
#include <cstdint>
#include "UI/UICommand.hpp"

namespace x11 {

class UICommandQueue {
public:
  bool push(const UICommand& c);

  // Optional wrappers for readability (NOT “per handler”; per command type)
  bool pushCreate(uint32_t xid, uint32_t parent, const char* title, int32_t w, int32_t h, uint32_t flags = 0) {
    return push(UICommand{UICommand::Type::Create, xid, parent, w, h, flags, 0, 0, title});
  }
  // ...others similarly...
  void drainOnServerThread(); // or better: drain(std::vector<UICommand>& out)
};
  
} // namespace x11
