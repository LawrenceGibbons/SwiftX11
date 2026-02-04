//
//  PropOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//


#include "PropOps.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "XProtoContext.hpp"
#include "ReplyWriter.hpp"
#include "ByteReader.hpp"
#include "WireLE.hpp"

namespace x11 {

// -----------------------------
// Minimal in-proc PropertyTable
// -----------------------------
class PropertyTable {
public:
  static PropertyTable& instance() {
    static PropertyTable t;
    return t;
  }

  struct Prop {
    uint32_t type = 0;    // Atom
    uint8_t  format = 0;  // 0/8/16/32
    std::vector<uint8_t> data;
  };

  bool get(uint32_t wid, uint32_t atom, Prop& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key(wid, atom));
    if (it == map_.end()) return false;
    out = it->second;
    return true;
  }

  void setReplace(uint32_t wid, uint32_t atom, uint32_t type, uint8_t format,
                  const uint8_t* bytes, std::size_t n) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& p = map_[key(wid, atom)];
    p.type = type;
    p.format = format;
    p.data.assign(bytes ? bytes : nullptr, bytes ? bytes + n : nullptr);
    cap_(p.data);
  }

  void setAppend(uint32_t wid, uint32_t atom, uint32_t type, uint8_t format,
                 const uint8_t* bytes, std::size_t n, bool append) {
    std::lock_guard<std::mutex> lock(mu_);
    auto k = key(wid, atom);

    auto it = map_.find(k);
    if (it == map_.end() || it->second.type != type || it->second.format != format) {
      // If absent or incompatible, treat as replace (matches your old C behavior).
      auto& p = map_[k];
      p.type = type;
      p.format = format;
      p.data.assign(bytes ? bytes : nullptr, bytes ? bytes + n : nullptr);
      cap_(p.data);
      return;
    }

    auto& p = it->second;
    if (!bytes || n == 0) return;

    if (append) {
      p.data.insert(p.data.end(), bytes, bytes + n);
    } else {
      p.data.insert(p.data.begin(), bytes, bytes + n);
    }
    cap_(p.data);
  }

  void erase(uint32_t wid, uint32_t atom) {
    std::lock_guard<std::mutex> lock(mu_);
    map_.erase(key(wid, atom));
  }

private:
  static uint64_t key(uint32_t wid, uint32_t atom) {
    return (uint64_t(wid) << 32) | uint64_t(atom);
  }

  static void cap_(std::vector<uint8_t>& v) {
    // Bring-up cap: 1 MiB (match your C version)
    constexpr std::size_t kMax = (1u << 20);
    if (v.size() > kMax) v.resize(kMax);
  }

  mutable std::mutex mu_;
  std::unordered_map<uint64_t, Prop> map_;
};

// -----------------------------
// PropOps wiring
// -----------------------------
PropOps::PropOps(XProtoRegistrar& reg) {
  reg.registerMajor(18, &PropOps::onMajor, this); // ChangeProperty
  reg.registerMajor(20, &PropOps::onMajor, this); // GetProperty
}

void PropOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<PropOps*>(user)->handle(ctx, dc);
}

void PropOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case 18: handleChangeProperty(ctx, dc.seq, dc.minor /*mode*/, dc.br); return;
    case 20: handleGetProperty(ctx, dc.seq, dc.minor /*deleteFlag*/, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[PropOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// -----------------------------
// ChangeProperty (major 18)
// mode: 0 Replace, 1 Prepend, 2 Append
// body (20 bytes + data):
//   CARD32 window
//   CARD32 property
//   CARD32 type
//   CARD8  format (8/16/32)
//   3 pad
//   CARD32 nUnits
//   data...
// -----------------------------
void PropOps::handleChangeProperty(XProtoContext& /*ctx*/, uint16_t /*seq*/, uint8_t mode, ByteReader& br) {
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t wid   = br.readU32();
  const uint32_t atom  = br.readU32();
  const uint32_t type  = br.readU32();
  const uint8_t  fmt   = br.readU8();
  br.skip(3); // pad
  const uint32_t nUnits = br.readU32();

  uint32_t unitBytes = 0;
  if (fmt == 8) unitBytes = 1;
  else if (fmt == 16) unitBytes = 2;
  else if (fmt == 32) unitBytes = 4;
  else {
    br.skip(br.remaining());
    return;
  }

  const uint64_t dataBytes64 = uint64_t(nUnits) * uint64_t(unitBytes);
  if (dataBytes64 > br.remaining()) {
    // malformed / truncated
    br.skip(br.remaining());
    return;
  }
  const std::size_t dataBytes = (std::size_t)dataBytes64;

  const uint8_t* data = br.ptr();
  br.skip(br.remaining()); // consume including pad

  // Apply
  if (mode == 1) {
    PropertyTable::instance().setAppend(wid, atom, type, fmt, data, dataBytes, /*append*/false);
  } else if (mode == 2) {
    PropertyTable::instance().setAppend(wid, atom, type, fmt, data, dataBytes, /*append*/true);
  } else {
    PropertyTable::instance().setReplace(wid, atom, type, fmt, data, dataBytes);
  }
}

// -----------------------------
// GetProperty (major 20)
// body (20 bytes):
//   CARD32 window
//   CARD32 property
//   CARD32 type (0 = AnyPropertyType)
//   CARD32 longOffset (4-byte units)
//   CARD32 longLength (4-byte units)
// reply:
//   rep[1]=format (0 if none)
//   rep[8..11]=type
//   rep[12..15]=bytesAfter
//   rep[16..19]=nItems
//   payload padded to 4
// deleteFlag is dc.minor (0/1)
// -----------------------------
void PropOps::handleGetProperty(XProtoContext& ctx, uint16_t seq, uint8_t deleteFlag, ByteReader& br) {
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t wid      = br.readU32();
  const uint32_t atom     = br.readU32();
  const uint32_t reqType  = br.readU32();
  const uint32_t longOff  = br.readU32();
  const uint32_t longLen  = br.readU32();
  br.skip(br.remaining());

  PropertyTable::Prop p{};
  const bool found = PropertyTable::instance().get(wid, atom, p);

  // “no such property”
  if (!found || p.format == 0 || p.data.empty()) {
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t,32>& rep) {
      rep[1] = 0;                       // format
      wire::wr32_le(rep.data()+8,  0); // type=None
      wire::wr32_le(rep.data()+12, 0); // bytesAfter
      wire::wr32_le(rep.data()+16, 0); // nItems
    });
    return;
  }

  // Type mismatch => empty (but property exists)
  if (reqType != 0 && p.type != reqType) {
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t,32>& rep) {
      rep[1] = 0;                       // format
      wire::wr32_le(rep.data()+8,  0); // type=None
      wire::wr32_le(rep.data()+12, 0); // bytesAfter
      wire::wr32_le(rep.data()+16, 0); // nItems
    });
    return;
  }

  uint32_t unitBytes = 0;
  if (p.format == 8) unitBytes = 1;
  else if (p.format == 16) unitBytes = 2;
  else if (p.format == 32) unitBytes = 4;
  else {
    // invalid stored property
    (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t,32>& rep) {
      rep[1] = 0;
      wire::wr32_le(rep.data()+8,  0);
      wire::wr32_le(rep.data()+12, 0);
      wire::wr32_le(rep.data()+16, 0);
    });
    return;
  }

  const uint32_t totalBytes = (uint32_t)std::min<std::size_t>(p.data.size(), 0xFFFFFFFFu);

  const uint64_t byteOff64  = uint64_t(longOff) * 4ull;
  const uint64_t maxBytes64 = uint64_t(longLen) * 4ull;

  uint32_t sendOff = (byteOff64 >= totalBytes) ? totalBytes : (uint32_t)byteOff64;
  uint32_t remainBytes = totalBytes - sendOff;

  uint32_t sendBytes = remainBytes;
  if (maxBytes64 < sendBytes) sendBytes = (uint32_t)maxBytes64;

  // For 16/32 formats: only whole items
  if (unitBytes > 1) {
    sendBytes -= (sendBytes % unitBytes);
  }

  const uint32_t bytesAfter = remainBytes - sendBytes;
  const uint32_t nItems = (unitBytes ? (sendBytes / unitBytes) : 0);

  const uint32_t padBytes = (sendBytes + 3u) & ~3u;
  const uint32_t lengthWords = padBytes / 4u;

  // Send header
  const bool okHdr = ctx.reply().sendReply32(seq, [&](std::array<uint8_t,32>& rep) {
    rep[1] = p.format;
    wire::wr32_le(rep.data()+4,  lengthWords);
    wire::wr32_le(rep.data()+8,  p.type);
    wire::wr32_le(rep.data()+12, bytesAfter);
    wire::wr32_le(rep.data()+16, nItems);
  });
  if (!okHdr) return;

  // Send payload + pad
  if (sendBytes) {
    (void)ctx.reply().sendPaddedBytes(p.data.data() + sendOff, sendBytes);
  }

  // Delete property if requested and we returned the entire property starting at offset 0
  if (deleteFlag && longOff == 0 && sendOff == 0 && sendBytes == totalBytes) {
    PropertyTable::instance().erase(wid, atom);
  }
}

} // namespace x11
