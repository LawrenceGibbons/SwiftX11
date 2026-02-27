//
//  ReplyWriter.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/23/26.
//

#include "Ops/ReplyWriter.hpp"
#include "Transport/XProtoTransport.hpp"
#include "Utils/WireLE.hpp"

namespace x11 {

  static inline bool requestHasReply(uint8_t major, uint8_t minor) {
    switch (major) {
      case 3:  return true;  // GetWindowAttributes
      case 14: return true;  // GetGeometry
      case 15: return true;  // QueryTree
      case 16: return true;  // InternAtom
      case 17: return true;  // GetAtomName
      case 20: return true;  // GetProperty
      case 21: return true;  // ListProperties
      case 23: return true;  // GetSelectionOwner
      case 26: return true;  // GrabPointer
      case 31: return true;  // GrabKeyboard
      case 38: return true;  // QueryPointer
      case 40: return true;  // TranslateCoords
      case 43: return true;  // GetInputFocus
      case 47: return true;  // QueryFont
      case 48: return true;  // QueryTextExtents
      case 49: return true;  // ListFonts
      case 50: return true;  // ListFontsWithInfo
      case 73: return true;  // GetImage
      case 83: return true;  // ListInstalledColormaps
      case 84: return true;  // AllocColor
      case 85: return true;  // AllocNamedColor
      case 86: return true;  // AllocColorCells
      case 87: return true;  // AllocColorPlanes
      case 91: return true;  // QueryColors
      case 92: return true;  // LookupColor
      case 97: return true;  // QueryBestSize
      case 98: return true;  // QueryExtension
      case 99: return true;  // ListExtensions
      case 101: return true; // GetKeyboardMapping
      case 103: return true; // GetKeyboardControl
      case 106: return true; // GetPointerControl
      case 108: return true; // GetScreenSaver
      case 110: return true; // ListHosts
      case 116: return true; // SetPointerMapping
      case 117: return true; // GetPointerMapping
      case 118: return true; // SetModifierMapping
      case 119: return true; // GetModifierMapping
      default:
        return false;        // everything else is void
    }
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
  wire::wr16_le(rep.data() + 2, seq);
  wire::wr32_le(rep.data() + 4, 0);  // length_words = 0 (no extra bytes)

  wire::wr32_le(rep.data() + 8, root);
  wire::wr16_le(rep.data() + 12, (uint16_t)x);      // INT16 on wire
  wire::wr16_le(rep.data() + 14, (uint16_t)y);      // INT16 on wire
  wire::wr16_le(rep.data() + 16, w);
  wire::wr16_le(rep.data() + 18, h);
  wire::wr16_le(rep.data() + 20, borderWidth);
  wire::wr16_le(rep.data() + 22, depth);

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
  wire::wr16_le(rep.data() + 2, seq);
  wire::wr32_le(rep.data() + 4, 0);  // length_words = 0

  wire::wr32_le(rep.data() + 8, focus);

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
  wire::wr16_le(rep.data() + 2, seq);
  wire::wr32_le(rep.data() + 4, 0);          // length_words
  wire::wr32_le(rep.data() + 8, atom);       // atom in reply body at bytes 8..11
  return sendReply32Bytes(rep.data());
}

  bool ReplyWriter::sendGetAtomNameReply(uint16_t seq, const char* name, uint16_t nameLen) {
    if (!name) { name = ""; nameLen = 0; }

    const uint32_t padded = (uint32_t)((nameLen + 3u) & ~3u);
    const uint32_t words  = padded / 4u;

    std::array<uint8_t, 32> rep{};
    rep.fill(0);
    rep[0] = 1;
    rep[2] = (uint8_t)(seq & 0xFF);
    rep[3] = (uint8_t)((seq >> 8) & 0xFF);
    // length_words
    rep[4] = (uint8_t)(words & 0xFF);
    rep[5] = (uint8_t)((words >> 8) & 0xFF);
    rep[6] = (uint8_t)((words >> 16) & 0xFF);
    rep[7] = (uint8_t)((words >> 24) & 0xFF);

    // nameLen in rep[8..9] (CARD16)
    rep[8] = (uint8_t)(nameLen & 0xFF);
    rep[9] = (uint8_t)((nameLen >> 8) & 0xFF);

#ifdef X11_TRACE_VERBOSE
  const uint32_t pad = (4u - (uint32_t(nameLen) & 3u)) & 3u;
  fprintf(stderr,
          "[SEND] GetAtomNameReply seq=%u nameLen=%u words=%u pad=%u first='%c%c%c%c'\n",
          (unsigned)seq,
          (unsigned)nameLen,
          (unsigned)words,
          (unsigned)pad,
          (nameLen > 0 ? name[0] : '.'),
          (nameLen > 1 ? name[1] : '.'),
          (nameLen > 2 ? name[2] : '.'),
          (nameLen > 3 ? name[3] : '.'));
#endif
    
    // payload is the raw name bytes (unpadded) - function will pad if needed
    return sendReplyWithPaddedPayload(rep.data(), nameLen ? name : nullptr, (size_t)nameLen);
  }  
  
bool ReplyWriter::sendReply32Bytes(const void* rep32) {
  return t_.sendReplyBytes(rep32, 32);
}

bool ReplyWriter::sendReplyWithPaddedPayload(const void* rep32,
                                             const void* payload,
                                             std::size_t payloadBytes)
{
#ifndef NDEBUG
  const uint8_t major = t_.last_request_major_;
  const uint8_t minor = t_.last_request_minor_;
  const uint16_t seq  = t_.last_request_seq_;

  if (!requestHasReply(major, minor)) {
    fprintf(stderr,
      "[PROTO BUG] Reply sent for NO-REPLY request "
      "major=%u minor=%u seq=%u\n",
      (unsigned)major, (unsigned)minor, (unsigned)seq
    );
    __builtin_trap();  // or assert(false)
  }
#endif

  // 1) Send 32-byte reply header
  if (!sendReply32Bytes(rep32)) return false;

  // 2) Send payload exactly
  if (payloadBytes && payload) {
    if (!t_.sendReplyBytes(payload, payloadBytes)) return false;
  }

  // 3) Pad ONLY if needed
  const std::size_t pad = (4 - (payloadBytes & 3)) & 3;
  if (pad) {
    static const uint8_t zeros[4] = {0,0,0,0};
    if (!t_.sendReplyBytes(zeros, pad)) return false;
  }

#ifdef X11_TRACE_VERBOSE
  fprintf(stderr,
          "[ReplyWriter] Query reply sent payload=%zu pad=%zu total=%zu\n",
          payloadBytes, pad, payloadBytes + pad);
#endif

  return true;
}  
  
bool ReplyWriter::sendBytes(const void* bytes, std::size_t n) {
  if (!bytes || n == 0) return true;
  return t_.sendReplyBytes(bytes, n);
}
  
bool ReplyWriter::sendReplyRaw(const void* rep, std::size_t nbytes) {
  return t_.sendReplyBytes(rep, nbytes);
}
  
} // namespace x11

