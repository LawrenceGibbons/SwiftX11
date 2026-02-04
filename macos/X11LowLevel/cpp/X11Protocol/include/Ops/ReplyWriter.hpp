//
//  ReplyOps.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/23/26.
//

#pragma once
#include <cstdint>
#include <array>
#include <cstddef>

#include "XProtoTransport.hpp"

namespace x11 {

class ReplyWriter {
public:
  explicit ReplyWriter(XProtoTransport& t) : t_(t) {}
    
  // GetGeometry reply is exactly 32 bytes, no extra data.
  // root: window id of root
  // x/y: 16-bit signed values on wire
  // w/h: 16-bit unsigned
  // borderWidth: 16-bit unsigned
  // depth: 16-bit unsigned (X11 uses CARD16 here)
  bool sendGetGeometryReply(uint16_t seq,
                            uint32_t root,
                            int16_t x, int16_t y,
                            uint16_t w, uint16_t h,
                            uint16_t borderWidth,
                            uint16_t depth);

  // GetInputFocus reply is exactly 32 bytes, no extra data.
  // revertTo: CARD8 (0=None, 1=PointerRoot, 2=Parent)
  // focus: WINDOW (XID)
  bool sendGetInputFocusReply(uint16_t seq,
                              uint8_t revertTo,
                              uint32_t focus);
  
  bool sendInternAtomReply(uint16_t seq, uint32_t atom);
  bool sendGetAtomNameReply(uint16_t seq, const char* name, uint16_t nameLen);
  
  bool sendPaddedBytes(const void* bytes, std::size_t n); // pads to 4 bytes
  bool sendReply32Bytes(const void* rep32);  // expects 32 bytes
  bool sendReplyWithPaddedPayload(const void* rep32,
                                 const void* payload,
                                 std::size_t payloadBytes);  // pads payload to 4
  
  
  template <typename FillFn>
  bool sendReply32(uint16_t seq, FillFn fill) {
    std::array<uint8_t, 32> rep{};
    rep.fill(0);

    rep[0] = 1;
    // rep[1] left for fill to set if needed
    // write seq
    rep[2] = (uint8_t)(seq & 0xFF);
    rep[3] = (uint8_t)((seq >> 8) & 0xFF);

    // length_words default 0 unless fill changes it
    rep[4] = rep[5] = rep[6] = rep[7] = 0;

    fill(rep);
    return t_.sendReplyBytes(rep.data(), rep.size());
  }  
  
private:
  XProtoTransport& t_;
};

} // namespace x11
