//
//  PointerOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/18/26.
//

// PointerOps.cpp
#include "PointerOps.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <Core/XProtoContext.hpp>     // adjust include to your ctx header
#include <Transport/XProtoTransport.hpp>   // adjust include path
#include <Utils/ByteReader.hpp>        // your ByteReader
#include "XProtoRegistrar.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/CoreKeymap.hpp"         // modifier map storage (shared with XKB)

namespace x11 {

static inline void put16le(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void put32le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

enum : uint8_t {
  MappingSuccess = 0,
  MappingBusy    = 1,
  MappingFailed  = 2,
};

// --- Server-global pointer mapping (physical->logical) -----------------
// If you already have a place to store this (ctx/server state), move it there.
static uint8_t g_ptrMapN = 7;
static std::array<uint8_t, 32> g_ptrMap = { 1,2,3,4,5,6,7 };

// -----------------------------
// Modifier mapping state (core 118/119)
// -----------------------------
// Storage lives in Core/CoreKeymap.cpp (v1.20.0.20) so the XKEYBOARD
// keymap builder can derive its modmap from the same rows.

static inline uint32_t pad4_u32(uint32_t nbytes) {
  return (nbytes + 3u) & ~3u;
}

// Be permissive for bring-up: accept zeros, duplicates, etc.
static inline bool validateModifierMap(const uint8_t* map, uint8_t n) {
  if (n == 0) return false;
  if (n > kCoreMaxKeysPerModifier) return false;
  if (!map) return false;
  return true;
}


static bool validatePointerMap(const uint8_t* map, uint8_t n) {
  if (!map || n == 0) return false;
  bool seen[256] = {false};
  for (uint8_t i = 0; i < n; i++) {
    uint8_t v = map[i];
    if (v == 0) continue;      // disabled allowed
    if (v > n) return false;   // IMPORTANT: protocol expects 0..n
    if (seen[v]) return false;
    seen[v] = true;
  }
  return true;
}


static void sendReplyHeader(XProtoTransport& t,
                            uint16_t seq,
                            uint8_t rep1,
                            uint32_t lengthWords)
{
  uint8_t hdr[32];
  std::memset(hdr, 0, sizeof(hdr));
  hdr[0] = 1;               // Reply
  hdr[1] = rep1;            // reply-specific byte
  put16le(&hdr[2], seq);
  put32le(&hdr[4], lengthWords);
  // bytes 8..31 are reply-specific; we leave zero unless needed
  (void)t.sendReplyBytes(hdr, sizeof(hdr));
}


// ----------------------------------------------------------------------

  PointerOps::PointerOps(XProtoRegistrar& reg)
  {
    reg.registerMajor(x11::opcode::SetPointerMapping,   &PointerOps::onMajor, this); // 116
    reg.registerMajor(x11::opcode::GetPointerMapping,   &PointerOps::onMajor, this); // 117
    reg.registerMajor(x11::opcode::SetModifierMapping,  &PointerOps::onMajor, this); // 118
    reg.registerMajor(x11::opcode::GetModifierMapping,  &PointerOps::onMajor, this); // 119
  }
void PointerOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc)
{
  ((PointerOps*)user)->handle(ctx, dc);
}

void PointerOps::handle(XProtoContext& ctx, DispatchContext& dc)
{
  const uint8_t  major = dc.major;
  const uint8_t  data  = dc.minor;   // for these requests, this is the "nmap" byte
  const uint16_t seq   = dc.seq;
  ByteReader&    br    = dc.br;

  // IMPORTANT: replies must go through sendReplyBytes (not sendAll)
  // Adjust accessor if your context stores transport differently.
  XProtoTransport& t = ctx.transport();

  switch (major) {
    case x11::opcode::GetPointerMapping: { // 117
      // No request body, but be defensive:
      if (br.remaining()) br.skip(br.remaining());

      uint8_t n = g_ptrMapN;
      if (n == 0) n = 1;
      if (n > (uint8_t)g_ptrMap.size()) n = (uint8_t)g_ptrMap.size();

      const uint32_t payloadBytes = pad4_u32((uint32_t)n);
      const uint32_t lenw         = payloadBytes / 4u;

      // Reply: rep1 = n, lengthWords = payloadBytes/4
      sendReplyHeader(t, seq, n, lenw);

      std::vector<uint8_t> payload(payloadBytes, 0);
      std::memcpy(payload.data(), g_ptrMap.data(), n);

      (void)t.sendReplyBytes(payload.data(), payload.size());
      return;
    }

    case x11::opcode::SetPointerMapping: { // 116
      const uint8_t n = data;

      const uint8_t* map = (n ? br.readBytes(n) : nullptr);
      br.align4();
      if (br.remaining()) br.skip(br.remaining());

      uint8_t status = MappingSuccess;

      if (n == 0 || n > g_ptrMap.size() || !validatePointerMap(map, n)) {
        status = MappingFailed;
      } else {
        g_ptrMapN = n;
        std::memset(g_ptrMap.data(), 0, g_ptrMap.size());
        std::memcpy(g_ptrMap.data(), map, n);
      }

      sendReplyHeader(t, seq, status, 0);
      return;
    }

      // ---- 118: SetModifierMapping ----
    case x11::opcode::SetModifierMapping: {
      const uint8_t n = data;               // numKeyPerModifier
      const uint32_t rawBytes = uint32_t(n) * 8u;

      const uint8_t* keys = (rawBytes ? br.readBytes(rawBytes) : nullptr);
      br.align4();
      if (br.remaining()) br.skip(br.remaining());

      uint8_t status = MappingSuccess;

      // (We don't model MappingBusy yet; so never return Busy.)
      if (!validateModifierMap(keys, n) || !setCoreModifierMap(keys, n)) {
        status = MappingFailed;
      }

      sendReplyHeader(t, seq, status, 0);
      return;
    }

    // ---- 119: GetModifierMapping ----
    case x11::opcode::GetModifierMapping: {
      if (br.remaining()) br.skip(br.remaining());

      uint8_t storedN = 0;
      const uint8_t* stored = coreModifierMap(storedN);

      uint8_t n = storedN;
      if (n == 0) n = 1;
      if (n > kCoreMaxKeysPerModifier) n = kCoreMaxKeysPerModifier;

      const uint32_t rawBytes     = uint32_t(n) * 8u;
      const uint32_t payloadBytes = pad4_u32(rawBytes);
      const uint32_t lenw         = payloadBytes / 4u;

      // Reply: rep1 = numKeyPerModifier, lengthWords = payloadBytes/4
      sendReplyHeader(t, seq, n, lenw);

      std::vector<uint8_t> payload(payloadBytes, 0);

      const uint8_t clampedStored = (storedN == 0 ? 1 :
                                    (storedN > kCoreMaxKeysPerModifier ? kCoreMaxKeysPerModifier : storedN));
      const uint32_t storedRaw = uint32_t(clampedStored) * 8u;

      const uint32_t toCopy = (rawBytes < storedRaw) ? rawBytes : storedRaw;
      if (toCopy) std::memcpy(payload.data(), stored, toCopy);

      (void)t.sendReplyBytes(payload.data(), payload.size());
      return;
    }

    default:
      // Not ours.
      break;
  }
}

} // namespace x11
