//
//  DrawableSurfaceRegistry.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/22/26.
//

#pragma once
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include "Core/SurfaceDesc.hpp"

namespace x11 {

class DrawableSurfaceRegistry {
public:
  void set(uint32_t xid, const SurfaceDesc& s);
  void clear(uint32_t xid);

  bool get(uint32_t xid, SurfaceDesc& out) const;
  bool has(uint32_t xid) const;

private:
  mutable std::mutex mu_;
  std::unordered_map<uint32_t, SurfaceDesc> map_;
};

} // namespace x11
