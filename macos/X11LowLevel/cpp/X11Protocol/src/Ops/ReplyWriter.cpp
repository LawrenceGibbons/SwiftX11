//
//  ReplyWriter.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/23/26.
//

#include "ReplyWriter.hpp"
#include "XProtoTransport.hpp"

namespace x11 {

static inline void wr16_le(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void wr32_le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

bool ReplyWriter::sendGetGeometryReply(uint16_t seq,
                                   uint32_t root,
                                   int16_t x, int16_t y,
                                   uint16_t w, uint16_t h,
                                   uint16_t borderWidth,
                                   uint16_t depth)
{
  std::array<uint8_t, 32> rep{};
  rep.fill(0);

  rep[0] = 1;                  // Reply
  // rep[1] unused
  wr16_le(rep.data() + 2, seq);
  wr32_le(rep.data() + 4, 0);  // length_words = 0 (no extra bytes)

  wr32_le(rep.data() + 8, root);
  wr16_le(rep.data() + 12, (uint16_t)x);      // INT16 on wire
  wr16_le(rep.data() + 14, (uint16_t)y);      // INT16 on wire
  wr16_le(rep.data() + 16, w);
  wr16_le(rep.data() + 18, h);
  wr16_le(rep.data() + 20, borderWidth);
  wr16_le(rep.data() + 22, depth);

  return sendReply32Bytes(rep.data());
}
  
bool ReplyWriter::sendGetInputFocusReply(uint16_t seq,
                                      uint8_t revertTo,
                                      uint32_t focus)
{
  std::array<uint8_t, 32> rep{};
  rep.fill(0);

  rep[0] = 1;                  // Reply
  rep[1] = revertTo;           // revertTo
  wr16_le(rep.data() + 2, seq);
  wr32_le(rep.data() + 4, 0);  // length_words = 0

  wr32_le(rep.data() + 8, focus);

  return t_.sendReplyBytes(rep.data(), rep.size());
}

  
bool ReplyWriter::sendPaddedBytes(const void* bytes, std::size_t n) {
  if (n == 0) return true;
  if (!t_.sendReplyBytes(bytes, n)) return false;

  const uint8_t zeros[4] = {0,0,0,0};
  const std::size_t pad = (4 - (n & 3)) & 3;
  if (pad) return t_.sendReplyBytes(zeros, pad);
  return true;
}

bool ReplyWriter::sendInternAtomReply(uint16_t seq, uint32_t atom) {
  std::array<uint8_t, 32> rep{};
  rep.fill(0);
  rep[0] = 1;
  wr16_le(rep.data() + 2, seq);
  wr32_le(rep.data() + 4, 0);          // length_words
  wr32_le(rep.data() + 8, atom);       // atom in reply body at bytes 8..11
  return sendReply32Bytes(rep.data());
}

bool ReplyWriter::sendGetAtomNameReply(uint16_t seq, const char* name, uint16_t nameLen) {
  if (!name) { name = ""; nameLen = 0; }

  const uint32_t padBytes = (uint32_t)((nameLen + 3u) & ~3u);
  const uint32_t extraWords = padBytes / 4u;

  std::array<uint8_t, 32> rep{};
  rep.fill(0);
  rep[0] = 1;
  wr16_le(rep.data() + 2, seq);
  wr32_le(rep.data() + 4, extraWords);

  // bytes 8..9 = nameLen
  wr16_le(rep.data() + 8, nameLen);

  return sendReplyWithPaddedPayload(rep.data(), name, nameLen);
  
}
  
  
bool ReplyWriter::sendReply32Bytes(const void* rep32) {
  return t_.sendReplyBytes(rep32, 32);
}

bool ReplyWriter::sendReplyWithPaddedPayload(const void* rep32,
                                            const void* payload,
                                            std::size_t payloadBytes) {
  if (!sendReply32Bytes(rep32)) return false;
  if (payloadBytes == 0) return true;
  return sendPaddedBytes(payload, payloadBytes); // already pads to 4
} 
  
} // namespace x11

