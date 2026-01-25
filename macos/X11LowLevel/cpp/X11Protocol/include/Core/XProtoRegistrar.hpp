//
//  XProtoRegistrar.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#pragma once
#include <cstdint>

namespace x11 {

class ByteReader;

struct DispatchContext {
  uint8_t  major = 0;
  uint8_t  minor = 0;
  uint16_t seq   = 0;
  ByteReader& br;
};

// Handler signature: module-thunk + opaque user pointer.
using HandlerFn = void (*)(void* user, class XProtoContext& ctx, DispatchContext& dc);

class XProtoRegistrar {
public:
  virtual ~XProtoRegistrar() = default;

  // Register one major opcode handler.
  virtual void registerMajor(uint8_t major, HandlerFn fn, void* user) = 0;
};

} // namespace x11
