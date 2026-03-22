//
//  PropOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//


#include "Ops/PropOps.hpp"
#include "Core/PropertyTable.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"
#include "Core/XConstants.hpp"
#include "Core/ClipboardAtoms.hpp"
#include "Ops/ReplyWriter.hpp"
#include "Utils/ByteReader.hpp"
#include "Utils/WireLE.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/ScreenLayout.hpp"
#include "Core/timestamp.hpp"

extern "C" void x11_ui_push_log(int level, const char* message);

// Send PropertyNotify (type 28) for ChangeProperty/DeleteProperty.
// X11 spec: PropertyNotify is delivered only to clients that selected
// PropertyChangeMask (bit 22) on the window's event_mask.
// Java AWT uses PropertyNotify on _SUNW_JAVA_AWT_TIME to extract
// the server timestamp needed for SetSelectionOwner.
static void sendPropertyNotify(x11::XProtoContext& ctx, uint32_t wid,
                               uint32_t atom, bool deleted) {
  // Only send if the window has PropertyChangeMask selected
  x11::WindowView wv{};
  if (!ctx.windows().snapshot(wid, wv)) return;
  if (!(wv.event_mask & x11::mask::PropertyChange)) return;

  uint8_t ev[32] = {};
  ev[0] = 28; // PropertyNotify
  x11::wire::wr16_le(ev + 2, ctx.transport().lastSeq());
  x11::wire::wr32_le(ev + 4, wid);           // window
  x11::wire::wr32_le(ev + 8, atom);          // atom
  x11::wire::wr32_le(ev + 12, x11_now_ms_monotonic()); // time
  ev[16] = deleted ? 1 : 0;             // state: 0=NewValue, 1=Deleted
  (void)ctx.transport().sendEvent32(wid, ev);
}

extern "C" void x11_ui_push_title(uint32_t xid, const char* title_utf8);
extern "C" void x11_ui_push_resize(uint32_t xid, int32_t w_px, int32_t h_px);
extern "C" void x11_ui_push_size_hints(uint32_t xid, int32_t min_w, int32_t min_h,
                                       int32_t max_w, int32_t max_h,
                                       int32_t inc_w, int32_t inc_h);
extern "C" void x11_ui_push_move(uint32_t xid, int32_t x_px, int32_t y_px);
extern "C" void x11_ui_push_window_type(uint32_t xid, uint32_t type_atom);
extern "C" void x11_ui_push_initial_state(uint32_t xid, uint32_t state);

namespace x11 {

// -----------------------------
// PropOps wiring
// -----------------------------
PropOps::PropOps(XProtoRegistrar& reg) {
  reg.registerMajor(x11::opcode::ChangeProperty,  &PropOps::onMajor, this); // 18 ChangeProperty
  reg.registerMajor(x11::opcode::DeleteProperty,   &PropOps::onMajor, this); // 19 DeleteProperty
  reg.registerMajor(x11::opcode::GetProperty,      &PropOps::onMajor, this); // 20 GetProperty
  reg.registerMajor(x11::opcode::ListProperties,   &PropOps::onMajor, this); // 21 ListProperties
  reg.registerMajor(x11::opcode::RotateProperties, &PropOps::onMajor, this); // 114 RotateProperties
}

void PropOps::onMajor(void* user, XProtoContext& ctx, DispatchContext& dc) {
  if (!user) { dc.br.skip(dc.br.remaining()); return; }
  static_cast<PropOps*>(user)->handle(ctx, dc);
}

void PropOps::handle(XProtoContext& ctx, DispatchContext& dc) {
  switch (dc.major) {
    case x11::opcode::ChangeProperty: handleChangeProperty(ctx, dc.seq, dc.minor /*mode*/, dc.br); return;
    case x11::opcode::DeleteProperty: handleDeleteProperty(ctx, dc.seq, dc.br); return;
    case x11::opcode::GetProperty   : handleGetProperty(ctx, dc.seq, dc.minor /*deleteFlag*/, dc.br); return;
    case x11::opcode::ListProperties:   handleListProperties(ctx, dc.seq, dc.br); return;
    case x11::opcode::RotateProperties: handleRotateProperties(ctx, dc.seq, dc.br); return;
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
void PropOps::handleChangeProperty(XProtoContext& ctx, uint16_t seq, uint8_t mode, ByteReader& br) {
  if (br.remaining() < 20) { br.skip(br.remaining()); return; }

  const uint32_t wid   = br.readU32();

  // Validate window exists (allow root XID 0 and 1)
  if (wid != 0 && wid != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(wid, tmp)) {
      br.skip(br.remaining());
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::ChangeProperty);
      return;
    }
  }
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

  // X11 spec: generate PropertyNotify on every property change
  sendPropertyNotify(ctx, wid, atom, /*deleted*/false);

#ifndef NDEBUG
  {
    uint32_t host = ctx.windows().topLevelAncestorOf(wid);
    if (host == 0) host = wid;
    if (host == wid) {
      fprintf(stderr, "[PROP_TOPLEVEL] wid=0x%08X atom=%u fmt=%u bytes=%zu\n",
              (unsigned)wid, (unsigned)atom, (unsigned)fmt, dataBytes);
    }
  }
#endif

  // Push title update to UI when WM_NAME or _NET_WM_NAME is set.
  // Only from the top-level window itself — child windows like "FocusProxy"
  // and "Content window" set their own WM_NAME which must NOT overwrite
  // the parent's real title (e.g. "Generate Output Products").
  if (atom == x11::atom::kWM_NAME || atom == x11::atom::k_NET_WM_NAME) {
    uint32_t host = ctx.windows().topLevelAncestorOf(wid);
    if (host == 0) host = wid;
    std::string title(reinterpret_cast<const char*>(data), dataBytes);
    {
      char buf[200];
      snprintf(buf, sizeof(buf),
               "[TITLE] wid=0x%08X host=0x%08X atom=%u \"%.*s\"%s\n",
               (unsigned)wid, (unsigned)host,
               (unsigned)atom,
               (int)(dataBytes > 80 ? 80 : dataBytes),
               reinterpret_cast<const char*>(data),
               (host != wid) ? " (child — skipped)" : "");
      x11_ui_push_log(1, buf);
    }
    if (host == wid) {
      x11_ui_push_title(host, title.c_str());
    }
  }

  // -----------------------------------------------------------------------
  // WM_NORMAL_HINTS (ICCCM §4.1.2.3): XSizeHints, 18 CARD32s (format=32)
  // Layout: [0]=flags [1]=pad_x [2]=pad_y [3]=pad_width [4]=pad_height
  //         [5]=min_width [6]=min_height [7]=max_width [8]=max_height
  //         [9]=width_inc [10]=height_inc [11-14]=aspect
  //         [15]=base_width [16]=base_height [17]=win_gravity
  // flags: USPosition=1 USSize=2 PPosition=4 PSize=8 PMinSize=0x10
  //        PMaxSize=0x20 PResizeInc=0x40 PAspect=0x80 PBaseSize=0x100 PWinGravity=0x200
  // -----------------------------------------------------------------------
  if (atom == x11::atom::kWM_NORMAL_HINTS && fmt == 32 && dataBytes >= 5 * 4) {
    const uint32_t* d32 = reinterpret_cast<const uint32_t*>(data);
    const uint32_t flags = d32[0];

    // Extract position hints (fields [1]-[2], used with USPosition/PPosition)
    int32_t pos_x = 0, pos_y = 0;
    bool has_position = false;
    if ((flags & (0x01 | 0x04)) && dataBytes >= 3 * 4) { // USPosition or PPosition
      pos_x = (int32_t)d32[1];
      pos_y = (int32_t)d32[2];
      has_position = true;
    }

    // Extract size constraints
    int32_t desired_w = 0, desired_h = 0;
    int32_t min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    int32_t inc_w = 0, inc_h = 0;

    if ((flags & 0x08) && dataBytes >= 5 * 4) {       // PSize
      desired_w = (int32_t)d32[3];
      desired_h = (int32_t)d32[4];
    }
    if ((flags & 0x10) && dataBytes >= 7 * 4) {       // PMinSize
      min_w = (int32_t)d32[5]; min_h = (int32_t)d32[6];
      if (min_w > desired_w) desired_w = min_w;
      if (min_h > desired_h) desired_h = min_h;
    }
    if ((flags & 0x20) && dataBytes >= 9 * 4) {       // PMaxSize
      max_w = (int32_t)d32[7]; max_h = (int32_t)d32[8];
    }
    if ((flags & 0x40) && dataBytes >= 11 * 4) {      // PResizeInc
      inc_w = (int32_t)d32[9]; inc_h = (int32_t)d32[10];
    }
    if ((flags & 0x100) && dataBytes >= 17 * 4) {     // PBaseSize
      int32_t bw = (int32_t)d32[15], bh = (int32_t)d32[16];
      if (bw > desired_w) desired_w = bw;
      if (bh > desired_h) desired_h = bh;
    }

    uint32_t host = ctx.windows().topLevelAncestorOf(wid);
    if (host == 0) host = wid;

    fprintf(stderr, "[WM_HINTS_DBG] xid=0x%08X host=0x%08X flags=0x%X desired=%dx%d min=%dx%d max=%dx%d pos=(%d,%d) hasPos=%d\n",
            (unsigned)wid, (unsigned)host, (unsigned)flags,
            (int)desired_w, (int)desired_h, (int)min_w, (int)min_h,
            (int)max_w, (int)max_h, (int)pos_x, (int)pos_y, (int)has_position);

    // If we have a desired size and the window should be resized, do it (WM role).
    //
    // Resize when:
    //   (a) window is tiny (<50px) — original case; or
    //   (b) window is at the WM floor size (200×100) and desired is LARGER —
    //       the floor was applied in CreateWindow for a 1×1 window, and now the
    //       client's WM_NORMAL_HINTS tells us the real intended size; or
    //   (c) window size doesn't match desired — the client explicitly set PSize
    //       or PMinSize and expects the WM to honour it.
    //
    // Don't resize if the desired size would shrink the window below the floor.
    bool did_resize = false;
    if (desired_w > 0 && desired_h > 0) {
      WindowView vw{};
      if (ctx.windows().snapshot(host, vw)) {
        const bool isTiny = (vw.w < 50 || vw.h < 50);
        const bool desiredLarger = (desired_w > (int32_t)vw.w || desired_h > (int32_t)vw.h);
        if (isTiny || desiredLarger) {
          // No minimum size floor — let the client choose any size.
          uint16_t nw = (uint16_t)std::min(desired_w, (int32_t)65535);
          uint16_t nh = (uint16_t)std::min(desired_h, (int32_t)65535);
          if (nw != vw.w || nh != vw.h) {
            ctx.windows().setGeometry(host, vw.x, vw.y, nw, nh);
            x11_ui_push_resize(host, (int32_t)nw, (int32_t)nh);
            did_resize = true;
          }
        }
      }
    }

    // Position the window.
    //   - If PPosition/USPosition is set, use that.
    //   - If we just resized from the floor (window was 1×1 → floor → real size)
    //     and no position hint was given, center on the primary monitor.
    //     This emulates standard WM placement for windows that were created tiny
    //     and depend on WM_NORMAL_HINTS for sizing (e.g., Vivado splash banner).
    if (has_position) {
      WindowView vw{};
      if (ctx.windows().snapshot(host, vw)) {
        ctx.windows().setGeometry(host, (int16_t)pos_x, (int16_t)pos_y, vw.w, vw.h);
        x11_ui_push_move(host, pos_x, pos_y);
      }
    } else if (did_resize) {
      // No explicit position — center on primary monitor.
      WindowView vw{};
      if (ctx.windows().snapshot(host, vw)) {
        auto layout = x11::getScreenLayout();
        // Find primary monitor (or first if none marked primary)
        const x11::MonitorInfo* primary = nullptr;
        for (auto& m : layout.monitors) {
          if (m.is_primary) { primary = &m; break; }
        }
        if (!primary && !layout.monitors.empty())
          primary = &layout.monitors[0];
        if (primary) {
          int32_t cx = primary->x + ((int32_t)primary->w - (int32_t)vw.w) / 2;
          int32_t cy = primary->y + ((int32_t)primary->h - (int32_t)vw.h) / 2;
          if (cx < primary->x) cx = primary->x;
          if (cy < primary->y) cy = primary->y;
          ctx.windows().setGeometry(host, (int16_t)cx, (int16_t)cy, vw.w, vw.h);
          x11_ui_push_move(host, cx, cy);
        }
      }
    }

    // Push size constraints to Swift for NSWindow contentMinSize/contentMaxSize/resizeIncrements
    x11_ui_push_size_hints(host, min_w, min_h, max_w, max_h, inc_w, inc_h);
  }

  // -----------------------------------------------------------------------
  // WM_HINTS (ICCCM §4.1.2.4): XWMHints, 9 CARD32s (format=32, atom=35)
  // Layout: [0]=flags [1]=input [2]=initial_state [3]=icon_pixmap
  //         [4]=icon_window [5]=icon_x [6]=icon_y [7]=icon_mask [8]=window_group
  // flags: InputHint=1 StateHint=2 IconPixmapHint=4 ...
  // -----------------------------------------------------------------------
  if (atom == x11::atom::kWM_HINTS && fmt == 32 && dataBytes >= 3 * 4) {
    const uint32_t* d32 = reinterpret_cast<const uint32_t*>(data);
    const uint32_t flags = d32[0];

    uint32_t host = ctx.windows().topLevelAncestorOf(wid);
    if (host == 0) host = wid;

    // InputHint: store whether client wants input focus
    if (flags & 1) {
      bool wants_input = (d32[1] != 0);
      ctx.windows().setWantsInput(host, wants_input);
    }

    // StateHint: initial_state (1=NormalState, 3=IconicState)
    if (flags & 2) {
      uint32_t initial_state = d32[2];
      x11_ui_push_initial_state(host, initial_state);
    }
  }

  // -----------------------------------------------------------------------
  // WM_PROTOCOLS: check for WM_TAKE_FOCUS and cache in WindowTable
  // -----------------------------------------------------------------------
  if (atom == x11::atom::kWM_PROTOCOLS && fmt == 32) {
    uint32_t host = ctx.windows().topLevelAncestorOf(wid);
    if (host == 0) host = wid;
    bool has_take_focus = false;
    bool has_delete_window = false;
    const uint32_t* d32 = reinterpret_cast<const uint32_t*>(data);
    const size_t nAtoms = dataBytes / 4;
    for (size_t i = 0; i < nAtoms; i++) {
      if (d32[i] == x11::atom::kWM_TAKE_FOCUS)    has_take_focus = true;
      if (d32[i] == x11::atom::kWM_DELETE_WINDOW)  has_delete_window = true;
    }
    ctx.windows().setWantsTakeFocus(host, has_take_focus);
  }

  // -----------------------------------------------------------------------
  // _NET_WM_WINDOW_TYPE (EWMH): atom value(s) → NSWindow style mapping
  // -----------------------------------------------------------------------
  if (atom == x11::atom::k_NET_WM_WINDOW_TYPE && fmt == 32 && dataBytes >= 4) {
    const uint32_t* d32 = reinterpret_cast<const uint32_t*>(data);
    uint32_t type_atom = d32[0]; // first (primary) type
    uint32_t host = ctx.windows().topLevelAncestorOf(wid);
    if (host == 0) host = wid;
    x11_ui_push_window_type(host, type_atom);
  }

  // -----------------------------------------------------------------------
  // _NET_WM_STATE (EWMH): modal, fullscreen, etc.
  // For now, just parse and push modal/fullscreen flags to Swift.
  // -----------------------------------------------------------------------
  if (atom == x11::atom::k_NET_WM_STATE && fmt == 32) {
    const uint32_t* d32 = reinterpret_cast<const uint32_t*>(data);
    const size_t nAtoms = dataBytes / 4;
    uint32_t host = ctx.windows().topLevelAncestorOf(wid);
    if (host == 0) host = wid;
    bool modal = false, fullscreen = false;
    for (size_t i = 0; i < nAtoms; i++) {
      if (d32[i] == x11::atom::k_NET_WM_STATE_MODAL)      modal = true;
      if (d32[i] == x11::atom::k_NET_WM_STATE_FULLSCREEN) fullscreen = true;
    }
    // Encode state flags in a window_type push: use a synthetic type atom
    // Modal → push as window type with flag 0x80000001
    // Fullscreen → push as window type with flag 0x80000002
    if (modal)      x11_ui_push_window_type(host, 0x80000001);
    if (fullscreen)  x11_ui_push_window_type(host, 0x80000002);
  }
}

// -----------------------------
// DeleteProperty (major 19)
// body: CARD32 window, CARD32 property
// -----------------------------
void PropOps::handleDeleteProperty(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  if (br.remaining() < 8) { br.skip(br.remaining()); return; }
  const uint32_t wid  = br.readU32();
  const uint32_t atom = br.readU32();
  br.skip(br.remaining());

  // Validate window exists (allow root XID 0 and 1)
  if (wid != 0 && wid != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(wid, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::DeleteProperty);
      return;
    }
  }

  PropertyTable::instance().erase(wid, atom);

  // X11 spec: DeleteProperty generates PropertyNotify with state=Deleted
  sendPropertyNotify(ctx, wid, atom, /*deleted*/true);

  ctx.tracef("[PropOps] DeleteProperty wid=0x%08X atom=%u\n", (unsigned)wid, (unsigned)atom);
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

  // Validate window exists (allow root XID 0 and 1)
  if (wid != 0 && wid != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(wid, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::GetProperty);
      return;
    }
  }

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
      rep[1] = 0;                          // format = 0
      wire::wr32_le(rep.data()+8,  p.type); // type = actual property type
      wire::wr32_le(rep.data()+12, 0);      // bytesAfter = 0
      wire::wr32_le(rep.data()+16, 0);      // nItems = 0
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

  const uint32_t paddedBytes = (sendBytes + 3u) & ~3u;
  const uint32_t lengthWords = paddedBytes / 4u;

  std::array<uint8_t,32> rep{};
  rep.fill(0);
  rep[0] = 1;
  rep[2] = (uint8_t)(seq & 0xFF);
  rep[3] = (uint8_t)((seq >> 8) & 0xFF);

  rep[1] = p.format;
  wire::wr32_le(rep.data()+4,  lengthWords);
  wire::wr32_le(rep.data()+8,  p.type);
  wire::wr32_le(rep.data()+12, bytesAfter);
  wire::wr32_le(rep.data()+16, nItems);

  // Build contiguous payload (unpadded), let ReplyWriter pad once if needed
  std::vector<uint8_t> payload;
  payload.resize(sendBytes);
  if (sendBytes) {
    memcpy(payload.data(), p.data.data() + sendOff, sendBytes);
  }

  const void* payloadPtr = payload.empty() ? nullptr : payload.data();
  const bool ok = ctx.reply().sendReplyWithPaddedPayload(rep.data(),
                                                        payloadPtr,
                                                        payload.size());
  if (!ok) return;
  
  // Delete property if requested and we returned the entire property starting at offset 0
  // X11 spec: only delete when entire property is returned (offset 0, no bytes remaining)
  if (deleteFlag && longOff == 0 && sendOff == 0 && bytesAfter == 0) {
    PropertyTable::instance().erase(wid, atom);
    // X11 spec: generate PropertyNotify with state=Deleted after deletion
    sendPropertyNotify(ctx, wid, atom, /*deleted*/true);
  }
}
  
  
void PropOps::handleListProperties(XProtoContext& ctx, uint16_t seq, ByteReader& br) {
  // Request body: WINDOW
  if (br.remaining() < 4) { br.skip(br.remaining()); return; }
  const uint32_t wid = br.readU32();
  if (br.remaining()) br.skip(br.remaining());

  // Validate window exists (allow root XID 0 and 1)
  if (wid != 0 && wid != x11::kRootXid) {
    WindowView tmp{};
    if (!ctx.windows().snapshot(wid, tmp)) {
      ctx.transport().sendErrorCore(x11::error::BadWindow, seq, wid, x11::opcode::ListProperties);
      return;
    }
  }

  std::vector<uint32_t> atoms;
  PropertyTable::instance().listAtoms(wid, atoms);

  const uint16_t n = (uint16_t)std::min<size_t>(atoms.size(), 0xFFFFu);
  const uint32_t payloadBytes = uint32_t(n) * 4u;     // CARD32 atoms
  const uint32_t lengthWords  = payloadBytes / 4u;    // == n

  std::array<uint8_t, 32> rep{};
  rep.fill(0);
  rep[0] = 1; // Reply
  // rep[1] unused/pad
  wire::wr16_le(rep.data() + 2, seq);
  wire::wr32_le(rep.data() + 4, lengthWords);
  wire::wr16_le(rep.data() + 8, n);  // nProperties

  // Build payload as packed LE CARD32s
  std::vector<uint8_t> payload(payloadBytes);
  for (uint16_t i = 0; i < n; i++) {
    wire::wr32_le(payload.data() + 4u * i, atoms[i]);
  }

  // Use ReplyWriter so your debug tracker stays coherent.
  // payloadBytes is already multiple-of-4 so padding is a no-op.
  (void)ctx.reply().sendReplyWithPaddedPayload(rep.data(),
                                              payload.empty() ? nullptr : payload.data(),
                                              payload.size());
}
  
  

// major 114 RotateProperties (void stub)
// Request: CARD32 window + CARD16 nProps + INT16 delta + nProps*CARD32 atoms
void PropOps::handleRotateProperties(XProtoContext& /*ctx*/, uint16_t /*seq*/, ByteReader& br) {
  br.skip(br.remaining());
}

} // namespace x11
