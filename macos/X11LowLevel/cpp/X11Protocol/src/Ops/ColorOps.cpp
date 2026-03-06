//
//  ColorOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/10/26.
//

#include "ColorOps.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include "Core/XConstants.hpp"
#include "ByteReader.hpp"
#include "ReplyWriter.hpp"
#include "WireLE.hpp"
#include "AtomTable.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Utils/X11ColorNames.hpp"

namespace x11 {

static constexpr uint32_t kRootCmap = 0x00000020u; // default colormap advertised in setup

// ------------------------------
// Minimal colormap state (bring-up)
// ------------------------------
struct ColormapState {
  // created colormaps
  std::unordered_set<uint32_t> created;

  // installed colormaps (ListInstalledColormaps)
  std::vector<uint32_t> installed; // keep order stable

  // simple pixel allocator (just a monotonically increasing 24-bit pixel)
  uint32_t next_pixel = 0x00010000;

  void ensureCreated(uint32_t cmap) {
    if (cmap == 0) return;
    created.insert(cmap);
  }

  void install(uint32_t cmap) {
    if (cmap == 0) return;
    ensureCreated(cmap);
    if (std::find(installed.begin(), installed.end(), cmap) == installed.end()) {
      installed.push_back(cmap);
    }
  }

  void uninstall(uint32_t cmap) {
    installed.erase(std::remove(installed.begin(), installed.end(), cmap), installed.end());
  }

  void free(uint32_t cmap) {
    created.erase(cmap);
    uninstall(cmap);
  }

  uint32_t allocPixel() {
    // keep it nonzero; wrap safely in 24-bit
    uint32_t p = next_pixel & 0x00FFFFFFu;
    if (p == 0) p = 1;
    next_pixel = ((next_pixel + 0x00010101u) & 0x00FFFFFFu);
    if (next_pixel == 0) next_pixel = 1;
    return p;
  }
};

static ColormapState& cmapState() {
  static ColormapState s;
  return s;
}
static std::mutex& cmapMu() {
  static std::mutex m;
  return m;
}

// ------------------------------
// Helpers
// ------------------------------
static inline uint32_t packRGB16ToPixel24(uint16_t r, uint16_t g, uint16_t b) {
  // Use the high byte of each 16-bit channel: RRGGBB.
  // r,g,b are 0..65535; take top 8 bits.
  return (uint32_t(r & 0xFF00u) << 8) | (uint32_t(g & 0xFF00u) << 0) | (uint32_t(b & 0xFF00u) >> 8);
}

// ------------------------------
// Registration / dispatch
// ------------------------------
ColorOps::ColorOps(XProtoRegistrar& reg) {
  for (uint8_t m = x11::opcode::CreateColormap; m <= x11::opcode::StoreNamedColor; ++m) {
    reg.registerMajor(m, &ColorOps::onMajor, this);
  }
  reg.registerMajor(x11::opcode::LookupColor, &ColorOps::onMajor, this);
}

void ColorOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  static_cast<ColorOps*>(user)->handle(ctx, dc);
}

void ColorOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::CreateColormap        : handleCreateColormap(ctx, dc.seq, dc.br); return;
    case x11::opcode::FreeColormap          : handleFreeColormap(ctx, dc.seq, dc.br); return;
    case x11::opcode::CopyColormapAndFree   : handleCopyColormapAndFree(ctx, dc.seq, dc.br); return;
    case x11::opcode::InstallColormap       : handleInstallColormap(ctx, dc.seq, dc.br); return;
    case x11::opcode::UninstallColormap     : handleUninstallColormap(ctx, dc.seq, dc.br); return;
    case x11::opcode::ListInstalledColormaps: handleListInstalledColormaps(ctx, dc.seq, dc.br); return;
    case x11::opcode::AllocColor            : handleAllocColor(ctx, dc.seq, dc.br); return;
    case x11::opcode::AllocNamedColor       : handleAllocNamedColor(ctx, dc.seq, dc.br); return;
    case x11::opcode::AllocColorCells       : handleAllocColorCells(ctx, dc.seq, dc.br); return;
    case x11::opcode::AllocColorPlanes      : handleAllocColorPlanes(ctx, dc.seq, dc.br); return;
    case x11::opcode::FreeColors            : handleFreeColors(ctx, dc.seq, dc.br); return;
    case x11::opcode::StoreColors           : handleStoreColors(ctx, dc.seq, dc.br); return;
    case x11::opcode::StoreNamedColor       : handleStoreNamedColor(ctx, dc.seq, dc.br); return;
    case x11::opcode::LookupColor           : handleLookupColor(ctx, dc.seq, dc.br); return;
    default:
      dc.br.skip(dc.br.remaining());
      ctx.tracef("[ColorOps] unexpected major=%u\n", (unsigned)dc.major);
      return;
  }
}

// ------------------------------
// 78 CreateColormap (no reply)
// req: alloc(CARD8 in major minor byte), depth? (actually: major byte0=alloc), pad,
//      mid(CARD32), window(CARD32), visual(CARD32)
void ColorOps::handleCreateColormap(XProtoContext& ctx, uint16_t, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }
  const uint32_t mid    = br.readU32();
  const uint32_t window = br.readU32();
  const uint32_t visual = br.readU32();
  br.skip(br.remaining());

  std::lock_guard<std::mutex> lock(cmapMu());
  cmapState().ensureCreated(mid);
  // Don’t auto-install; client calls InstallColormap if it cares.
  ctx.tracef("[ColorOps] CreateColormap mid=0x%08X window=0x%08X visual=0x%08X\n",
             (unsigned)mid, (unsigned)window, (unsigned)visual);
}

// 79 FreeColormap (no reply) req: cmap(CARD32)
void ColorOps::handleFreeColormap(XProtoContext& ctx, uint16_t, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t cmap = br.readU32();
  br.skip(br.remaining());

  std::lock_guard<std::mutex> lock(cmapMu());
  cmapState().free(cmap);
  ctx.tracef("[ColorOps] FreeColormap cmap=0x%08X\n", (unsigned)cmap);
}

// 80 CopyColormapAndFree (no reply) req: mid(CARD32), src(CARD32)
void ColorOps::handleCopyColormapAndFree(XProtoContext& ctx, uint16_t, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }
  const uint32_t mid = br.readU32();
  const uint32_t src = br.readU32();
  br.skip(br.remaining());

  std::lock_guard<std::mutex> lock(cmapMu());
  cmapState().ensureCreated(mid);
  // bring-up: treat as create + free src (common semantics)
  cmapState().free(src);
  ctx.tracef("[ColorOps] CopyColormapAndFree mid=0x%08X src=0x%08X\n", (unsigned)mid, (unsigned)src);
}

// 81 InstallColormap (no reply) req: cmap(CARD32)
void ColorOps::handleInstallColormap(XProtoContext& ctx, uint16_t, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t cmap = br.readU32();
  br.skip(br.remaining());

  std::lock_guard<std::mutex> lock(cmapMu());
  cmapState().install(cmap);
  ctx.tracef("[ColorOps] InstallColormap cmap=0x%08X\n", (unsigned)cmap);
}

// 82 UninstallColormap (no reply) req: cmap(CARD32)
void ColorOps::handleUninstallColormap(XProtoContext& ctx, uint16_t, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t cmap = br.readU32();
  br.skip(br.remaining());

  std::lock_guard<std::mutex> lock(cmapMu());
  cmapState().uninstall(cmap);
  ctx.tracef("[ColorOps] UninstallColormap cmap=0x%08X\n", (unsigned)cmap);
}

// 83 ListInstalledColormaps (reply) req: window(CARD32)
// reply: rep[1]=n, length_words=n, payload=LISTofCARD32 colormaps
void ColorOps::handleListInstalledColormaps(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t window = br.readU32();
  br.skip(br.remaining());

  // Validate window exists (allow root XID 0 and 1)
  if (window != 0 && window != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(window, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, window, x11::opcode::ListInstalledColormaps);
      return;
    }
  }

  std::vector<uint32_t> list;
  {
    std::lock_guard<std::mutex> lock(cmapMu());
    list = cmapState().installed;
  }
  // If none installed, return empty list (legal).
  const uint32_t n = (uint32_t)list.size();
  const uint32_t payloadBytes = n * 4u;
  const uint32_t payloadWords = payloadBytes / 4u;

  const bool ok = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    rep[1] = (uint8_t)std::min<uint32_t>(n, 255u);
    wire::wr32_le(rep.data() + 4, payloadWords);
    wire::wr16_le(rep.data() + 8, (uint16_t)n); // some implementations use rep[8..9] for n (not required)
  });
  if (!ok) return;

  if (payloadBytes) {
    // send list as raw bytes (already 4-aligned)
    (void)ctx.reply().sendPaddedBytes(list.data(), payloadBytes);
  }

  ctx.tracef("[ColorOps] ListInstalledColormaps window=0x%08X n=%u\n", (unsigned)window, (unsigned)n);
}

// 84 AllocColor (reply)
// req: cmap(CARD32) r(CARD16) g(CARD16) b(CARD16) pad(CARD16)
// reply: pixel(CARD32), r,g,b(CARD16), pad(CARD16) length=0
void ColorOps::handleAllocColor(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }
  const uint32_t cmap = br.readU32();
  const uint16_t r = br.readU16();
  const uint16_t g = br.readU16();
  const uint16_t b = br.readU16();
  br.skip(2);
  br.skip(br.remaining());

  // Validate colormap
  if (cmap != kRootCmap) {
    std::lock_guard<std::mutex> lock(cmapMu());
    if (cmapState().created.find(cmap) == cmapState().created.end()) {
      ctx.transport().sendErrorCore(x11::error::BadColor, seq, cmap, x11::opcode::AllocColor);
      return;
    }
  }

  uint32_t pixel = 0;
  {
    std::lock_guard<std::mutex> lock(cmapMu());
    cmapState().ensureCreated(cmap);
    // For bring-up: deterministic mapping for same rgb isn’t required.
    pixel = packRGB16ToPixel24(r, g, b);
    if (pixel == 0) pixel = cmapState().allocPixel();
  }

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    wire::wr32_le(rep.data() + 8, pixel);
    wire::wr16_le(rep.data() + 12, r);
    wire::wr16_le(rep.data() + 14, g);
    wire::wr16_le(rep.data() + 16, b);
  });

  ctx.tracef("[ColorOps] AllocColor cmap=0x%08X rgb=%u,%u,%u -> pixel=0x%08X\n",
             (unsigned)cmap, (unsigned)r, (unsigned)g, (unsigned)b, (unsigned)pixel);
}

// 85 AllocNamedColor (reply)
// req: cmap(CARD32), nameLen(CARD16), pad(CARD16), name bytes + pad
// reply: pixel(CARD32), exactRGB(3*CARD16), screenRGB(3*CARD16), length=0
void ColorOps::handleAllocNamedColor(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }
  const uint32_t cmap = br.readU32();
  const uint16_t nbytes = br.readU16();
  br.skip(2);

  // Validate colormap (read name first so bytes are consumed even on error)
  std::string name;
  if (const uint8_t* p = br.peekBytes(nbytes)) name.assign((const char*)p, nbytes);
  br.skip(std::min<std::size_t>(br.remaining(), nbytes));
  br.skip(br.remaining());

  if (cmap != kRootCmap) {
    std::lock_guard<std::mutex> lock(cmapMu());
    if (cmapState().created.find(cmap) == cmapState().created.end()) {
      ctx.transport().sendErrorCore(x11::error::BadColor, seq, cmap, x11::opcode::AllocNamedColor);
      return;
    }
  }

  uint16_t r = 0, g = 0, b = 0;
  uint8_t r8 = 255, g8 = 255, b8 = 255;
  if (x11::lookupColorName(name.c_str(), name.size(), r8, g8, b8)) {
    r = (uint16_t(r8) << 8) | r8;
    g = (uint16_t(g8) << 8) | g8;
    b = (uint16_t(b8) << 8) | b8;
  } else {
    // Unknown -> white fallback (bring-up)
    r = g = b = 0xFFFF;
  }

  uint32_t pixel = 0;
  {
    std::lock_guard<std::mutex> lock(cmapMu());
    cmapState().ensureCreated(cmap);
    pixel = packRGB16ToPixel24(r, g, b);
    if (pixel == 0) pixel = cmapState().allocPixel();
  }

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    wire::wr32_le(rep.data() + 8, pixel);

    // exact
    wire::wr16_le(rep.data() + 12, r);
    wire::wr16_le(rep.data() + 14, g);
    wire::wr16_le(rep.data() + 16, b);

    // screen (same as exact for bring-up)
    wire::wr16_le(rep.data() + 18, r);
    wire::wr16_le(rep.data() + 20, g);
    wire::wr16_le(rep.data() + 22, b);
  });

  ctx.tracef("[ColorOps] AllocNamedColor cmap=0x%08X name=\"%s\" -> pixel=0x%08X\n",
             (unsigned)cmap, name.c_str(), (unsigned)pixel);
}

// 86 AllocColorCells (reply)
// req: cmap(CARD32), contiguous(CARD8), pad, colors(CARD16), planes(CARD16)
// reply: rep[1]=0, length_words=(nPixels+nMasks), nPixels(CARD16) at rep[8..9], nMasks(CARD16) at rep[10..11]
// payload: pixels[nPixels] (CARD32) + masks[nMasks] (CARD32)
void ColorOps::handleAllocColorCells(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }
  const uint32_t cmap = br.readU32();
  const uint8_t contiguous = br.readU8();
  br.skip(1);
  const uint16_t nColors = br.readU16();
  const uint16_t nPlanes = br.readU16();
  br.skip(br.remaining());

  (void)contiguous;

  std::vector<uint32_t> pixels;
  std::vector<uint32_t> masks;

  {
    std::lock_guard<std::mutex> lock(cmapMu());
    cmapState().ensureCreated(cmap);
    pixels.reserve(nColors);
    for (uint16_t i = 0; i < nColors; ++i) pixels.push_back(cmapState().allocPixel());

    // bring-up: return masks=0 (valid but not very useful). Still must return nPlanes masks.
    masks.resize(nPlanes, 0);
  }

  const uint32_t payloadBytes = (uint32_t)(pixels.size() * 4u + masks.size() * 4u);
  const uint32_t payloadWords = payloadBytes / 4u;

  const bool ok = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    wire::wr32_le(rep.data() + 4, payloadWords);
    wire::wr16_le(rep.data() + 8, (uint16_t)pixels.size());
    wire::wr16_le(rep.data() + 10, (uint16_t)masks.size());
  });
  if (!ok) return;

  if (!pixels.empty()) (void)ctx.reply().sendPaddedBytes(pixels.data(), pixels.size() * 4u);
  if (!masks.empty())  (void)ctx.reply().sendPaddedBytes(masks.data(), masks.size() * 4u);

  ctx.tracef("[ColorOps] AllocColorCells cmap=0x%08X colors=%u planes=%u\n",
             (unsigned)cmap, (unsigned)nColors, (unsigned)nPlanes);
}

// 87 AllocColorPlanes (reply)
// req: cmap(CARD32), contiguous(CARD8), pad, colors(CARD16), reds(CARD16), greens(CARD16), blues(CARD16)
// reply: rep length_words = nPixels + 3 (pixels + 3 masks), rep[8..9]=nPixels
// payload: pixels[nPixels] (CARD32), redMask, greenMask, blueMask (CARD32 each)
void ColorOps::handleAllocColorPlanes(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 12) { br.skip(br.remaining()); return; }
  const uint32_t cmap = br.readU32();
  const uint8_t contiguous = br.readU8();
  br.skip(1);
  const uint16_t nColors = br.readU16();
  const uint16_t nR = br.readU16();
  const uint16_t nG = br.readU16();
  const uint16_t nB = br.readU16();
  br.skip(br.remaining());

  (void)contiguous;
  (void)nR; (void)nG; (void)nB;

  std::vector<uint32_t> pixels;
  uint32_t redMask=0, greenMask=0, blueMask=0;

  {
    std::lock_guard<std::mutex> lock(cmapMu());
    cmapState().ensureCreated(cmap);
    pixels.reserve(nColors);
    for (uint16_t i = 0; i < nColors; ++i) pixels.push_back(cmapState().allocPixel());

    // bring-up: masks=0 (valid-ish for some clients; xterm usually doesn’t need planes on TrueColor)
    redMask = greenMask = blueMask = 0;
  }

  const uint32_t payloadBytes = (uint32_t)(pixels.size() * 4u + 12u);
  const uint32_t payloadWords = payloadBytes / 4u;

  const bool ok = ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    wire::wr32_le(rep.data() + 4, payloadWords);
    wire::wr16_le(rep.data() + 8, (uint16_t)pixels.size());
    wire::wr32_le(rep.data() + 12, redMask);
    wire::wr32_le(rep.data() + 16, greenMask);
    wire::wr32_le(rep.data() + 20, blueMask);
  });
  if (!ok) return;

  if (!pixels.empty()) (void)ctx.reply().sendPaddedBytes(pixels.data(), pixels.size() * 4u);

  uint32_t masks[3] = { redMask, greenMask, blueMask };
  (void)ctx.reply().sendPaddedBytes(masks, sizeof(masks));

  ctx.tracef("[ColorOps] AllocColorPlanes cmap=0x%08X colors=%u\n", (unsigned)cmap, (unsigned)nColors);
}

// 88 FreeColors (no reply)
// req: cmap(CARD32), planeMask(CARD32), nPixels(CARD16 in length?), pixels list (CARD32*)
// We just consume bytes.
void ColorOps::handleFreeColors(XProtoContext& ctx, uint16_t, ByteReader& br) {
  // spec request is variable; just consume safely.
  br.skip(br.remaining());
  ctx.tracef("[ColorOps] FreeColors (noop)\n");
}

// 89 StoreColors (no reply) variable length list of xColorItem
void ColorOps::handleStoreColors(XProtoContext& ctx, uint16_t, ByteReader& br) {
  br.skip(br.remaining());
  ctx.tracef("[ColorOps] StoreColors (noop)\n");
}

// 90 StoreNamedColor (no reply)
// req: flags(CARD8 in minor/data), pad, cmap(CARD32), pixel(CARD32), nameLen(CARD16), pad, name bytes
void ColorOps::handleStoreNamedColor(XProtoContext& ctx, uint16_t, ByteReader& br) {
  br.skip(br.remaining());
  ctx.tracef("[ColorOps] StoreNamedColor (noop)\n");
}


// 92 LookupColor (reply)
// req: cmap(CARD32), nameLen(CARD16), pad(CARD16), name bytes + pad
// reply: exactRGB(3*CARD16), screenRGB(3*CARD16), length=0
void ColorOps::handleLookupColor(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }
  const uint32_t cmap = br.readU32();
  const uint16_t nbytes = br.readU16();
  br.skip(2);

  std::string name;
  if (const uint8_t* p = br.peekBytes(nbytes)) name.assign((const char*)p, nbytes);
  br.skip(std::min<std::size_t>(br.remaining(), nbytes));
  br.skip(br.remaining());

  // Validate colormap
  if (cmap != kRootCmap) {
    std::lock_guard<std::mutex> lock(cmapMu());
    if (cmapState().created.find(cmap) == cmapState().created.end()) {
      ctx.transport().sendErrorCore(x11::error::BadColor, seq, cmap, x11::opcode::LookupColor);
      return;
    }
  }

  uint8_t r8 = 0, g8 = 0, b8 = 0;
  if (!x11::lookupColorName(name.c_str(), name.size(), r8, g8, b8)) {
    // Unknown color -> return white
    r8 = g8 = b8 = 255;
  }

  const uint16_t r = (uint16_t(r8) << 8) | r8;
  const uint16_t g = (uint16_t(g8) << 8) | g8;
  const uint16_t b = (uint16_t(b8) << 8) | b8;

  (void)ctx.reply().sendReply32(seq, [&](std::array<uint8_t, 32>& rep) {
    // exact RGB
    wire::wr16_le(rep.data() + 8,  r);
    wire::wr16_le(rep.data() + 10, g);
    wire::wr16_le(rep.data() + 12, b);
    // screen RGB (same as exact for bring-up)
    wire::wr16_le(rep.data() + 14, r);
    wire::wr16_le(rep.data() + 16, g);
    wire::wr16_le(rep.data() + 18, b);
  });

  ctx.tracef("[ColorOps] LookupColor cmap=0x%08X name=\"%s\" -> rgb=%u,%u,%u\n",
             (unsigned)cmap, name.c_str(), (unsigned)r8, (unsigned)g8, (unsigned)b8);
}

} // namespace x11
