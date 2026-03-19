//
//  XProtoServer.cpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include <cstdio>
#include <unordered_map>
#include <exception>

#include "Core/XProtoServer.hpp"
#include "Core/XClient.hpp"
#include "Core/SurfaceDesc.hpp"
#include "Core/X11CoreOpcodes.hpp"
#include "Core/PropertyTable.hpp"
#include "Core/ClipboardAtoms.hpp"
#include "Core/ScreenLayout.hpp"
#include "Utils/ByteReader.hpp"
#include "Ops/EventOps.hpp"
#include "Core/timestamp.hpp"
#include "Utils/WireLE.hpp"

extern "C" {
#include "SwiftX11Bridge.h"
}

namespace x11 {

struct XProtoServer::Impl {
  std::unordered_map<uint32_t, WindowView> testWindows;
};

XProtoServer::XProtoServer()
  : ctx_()
  , eventOps_(std::make_unique<EventOps>(ctx_))
  , impl_(std::make_unique<Impl>())
{
  table_.fill(Entry{nullptr, nullptr});

  // Wire server-wide state into context.
  ctx_.setWindowTable(&windows_);
  ctx_.setPixmapTable(&pixmapTable_);
  ctx_.setUI(&ui_);
  ctx_.setFontTable(&fonts_);
  ctx_.setCursorTable(&cursors_);
  ctx_.setSurfaceRegistry(&surfaces_);
  ctx_.setGrabTable(&grabs_);
  ctx_.setInputState(&input_);

  // Default: context window lookup calls back into this instance.
  ctx_.setWindowLookup(&XProtoServer::lookupWindowTrampoline, this);

  // Wire pending-map callback so MapWindow handler can defer maps.
  ctx_.setPendingMapCallback(&XProtoServer::pendingMapTrampoline, this);

  // Wire peak-size callback so ConfigureWindow can track pre-map sizes.
  ctx_.setPeakSizeCallback(&XProtoServer::peakSizeTrampoline, this);

  // Load built-in fonts.
  std::string err;
  if (!fonts_.loadBuiltins(&err)) {
    ctx_.tracef("[FontTable] loadBuiltins failed: %s\n", err.c_str());
  }

}

x11::XProtoServer::~XProtoServer() = default;


// Reply-bearing core opcode table (X11 protocol spec).
// If a core opcode generates a reply, it MUST be sent — missing replies
// cause XCB sequence number desync ("Unknown sequence number" crash).
static bool isReplyBearingCore(uint8_t major) {
  static const bool kTable[128] = {
    // 0: unused
    false,
    // 1-13: window ops (all void except 3=GetWindowAttributes)
    false, false, true, false, false, false, false,
    false, false, false, false, false, false,
    // 14-15: GetGeometry(R), QueryTree(R)
    true, true,
    // 16-17: InternAtom(R), GetAtomName(R)
    true, true,
    // 18-19: ChangeProperty(V), DeleteProperty(V)
    false, false,
    // 20-21: GetProperty(R), ListProperties(R)
    true, true,
    // 22-24: SetSelectionOwner(V), GetSelectionOwner(R), ConvertSelection(V)
    false, true, false,
    // 25: SendEvent(V)
    false,
    // 26-27: GrabPointer(R), UngrabPointer(V)
    true, false,
    // 28-30: GrabButton(V), UngrabButton(V), ChangeActivePointerGrab(V)
    false, false, false,
    // 31-32: GrabKeyboard(R), UngrabKeyboard(V)
    true, false,
    // 33-37: GrabKey(V), UngrabKey(V), AllowEvents(V), GrabServer(V), UngrabServer(V)
    false, false, false, false, false,
    // 38-44: QueryPointer(R), GetMotionEvents(R), TranslateCoords(R),
    //        WarpPointer(V), SetInputFocus(V), GetInputFocus(R), QueryKeymap(R)
    true, true, true, false, false, true, true,
    // 45-46: OpenFont(V), CloseFont(V)
    false, false,
    // 47-50: QueryFont(R), QueryTextExtents(R), ListFonts(R), ListFontsWithInfo(R)
    true, true, true, true,
    // 51-52: SetFontPath(V), GetFontPath(R)
    false, true,
    // 53-72: Pixmap/GC/draw ops (all void except 73=GetImage)
    false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false,
    false, false, false, false,
    // 73: GetImage(R)
    true,
    // 74-77: PolyText8/16, ImageText8/16 (all void)
    false, false, false, false,
    // 78-82: Colormap ops (all void)
    false, false, false, false, false,
    // 83-87: ListInstalledColormaps(R), AllocColor(R), AllocNamedColor(R),
    //        AllocColorCells(R), AllocColorPlanes(R)
    true, true, true, true, true,
    // 88-90: FreeColors(V), StoreColors(V), StoreNamedColor(V)
    false, false, false,
    // 91-92: QueryColors(R), LookupColor(R)
    true, true,
    // 93-96: CreateCursor(V), CreateGlyphCursor(V), FreeCursor(V), RecolorCursor(V)
    false, false, false, false,
    // 97-99: QueryBestSize(R), QueryExtension(R), ListExtensions(R)
    true, true, true,
    // 100-102: ChangeKeyboardMapping(V), GetKeyboardMapping(R), ChangeKeyboardControl(V)
    false, true, false,
    // 103-105: GetKeyboardControl(R), Bell(V), ChangePointerControl(V)
    true, false, false,
    // 106-107: GetPointerControl(R), SetScreenSaver(V)
    true, false,
    // 108-109: GetScreenSaver(R), ChangeHosts(V)
    true, false,
    // 110-115: ListHosts(R), SetAccessControl(V), SetCloseDownMode(V),
    //          KillClient(V), RotateProperties(V), ForceScreenSaver(V)
    true, false, false, false, false, false,
    // 116-119: SetPointerMapping(R!), GetPointerMapping(R),
    //          SetModifierMapping(R!), GetModifierMapping(R)
    true, true, true, true,
    // 120-126: unused / reserved
    false, false, false, false, false, false, false,
    // 127: NoOperation(V)
    false,
  };
  return (major < 128) ? kTable[major] : false;
}

// ----------------------------------------------
int XProtoServer::dispatch(uint8_t major, uint8_t minor, uint16_t seq,
                           const uint8_t* payload, std::size_t remain)
{
  ByteReader br(payload, remain);

#ifdef X11_TRACE_VERBOSE
  if (major == 70 || major == 68 || major == 71 || major == 62 ||
      major == 63 || major == 72 || major == 74 || major == 75 ||
      major == 76 || major == 77 || major == 65 || major == 66)
  {
    fprintf(stderr, "[WATCH] major=%u minor=%u seq=%u remain=%zu\n",
            (unsigned)major, (unsigned)minor, (unsigned)seq, br.remaining());
  }
  fprintf(stderr, "[DISPATCH] major=%u minor=%u seq=%u remain=%zu\n",
          (unsigned)major, (unsigned)minor, (unsigned)seq, remain);
#endif
  if ( major == 91 ) {
#ifdef X11_TRACE_VERBOSE
    {
      const uint8_t* p = br.ptr();
      const size_t n = br.remaining();
      if (n >= 12) {
        uint32_t cmap_le = rd32_le(p+0), cmap_be = rd32_be(p+0);
        uint16_t n_le    = rd16_le(p+4), n_be    = rd16_be(p+4);
        uint32_t pix_le  = rd32_le(p+8), pix_be  = rd32_be(p+8);

        fprintf(stderr,
                "[QC SANITY] seq=%u raw=%02X%02X%02X%02X %02X%02X %02X%02X %02X%02X%02X%02X "
                "cmap(le=%08X be=%08X) n(le=%u be=%u) pix(le=%08X be=%08X)\n",
                (unsigned)seq,
                p[0],p[1],p[2],p[3], p[4],p[5], p[6],p[7], p[8],p[9],p[10],p[11],
                (unsigned)cmap_le, (unsigned)cmap_be,
                (unsigned)n_le, (unsigned)n_be,
                (unsigned)pix_le, (unsigned)pix_be
                );
      }
    }
    {
      const uint8_t* p = br.ptr();
      if (br.remaining() >= 6) {
        ByteReader tmp = br;
        tmp.skip(4);
        uint16_t v = tmp.readU16();
        fprintf(stderr, "[QC READU16] bytes=%02X%02X -> readU16()=%u\n",
                p[4], p[5], (unsigned)v);
      }
    }
#endif
  }


  const Entry& e = table_[major];
  // Track last request in the transport (if a client is connected).
  if (ctx_.hasClient()) {
    ctx_.transport().last_request_major_ = major;
    ctx_.transport().last_request_minor_ = minor;
    ctx_.transport().last_request_seq_   = seq;
  }
  // Reset reply-sent tracking before dispatch so we can detect missing replies.
  if (ctx_.hasClient()) {
    ctx_.transport().resetReplySent();
  }

  if (!e.fn) {
    {
      char buf[128];
      snprintf(buf, sizeof(buf),
               "[DISPATCH] UNHANDLED major=%u minor=%u seq=%u (no C++ handler)\n",
               (unsigned)major, (unsigned)minor, (unsigned)seq);
      x11_ui_push_log(1, buf);
    }
    // Safety net: if this was a reply-bearing opcode, send an error reply
    // to prevent XCB sequence desync crash.
    if (ctx_.hasClient() && isReplyBearingCore(major)) {
      char buf[128];
      snprintf(buf, sizeof(buf),
               "[DISPATCH] MISSING REPLY — sending BadImplementation for major=%u seq=%u\n",
               (unsigned)major, (unsigned)seq);
      x11_ui_push_log(0, buf);
      ctx_.transport().sendErrorCore(x11::error::BadImplementation, seq, 0, major);
    }
    return 0;
  }

  DispatchContext dc{
    .major = major,
    .minor = minor,
    .seq   = seq,
    .br    = br
  };

  try {
    e.fn(e.user, ctx_, dc);

    if (dc.br.remaining() > 0) {
      dc.br.skip(dc.br.remaining());
    }

    // Safety net: if opcode is reply-bearing and no reply was sent, send error.
    // This catches handlers that early-return without sending a reply (e.g.,
    // when br.remaining() < required bytes), preventing XCB sequence desync.
    if (ctx_.hasClient() && isReplyBearingCore(major) && !ctx_.transport().wasReplySent()) {
      fprintf(stderr,
              "[DISPATCH] MISSING REPLY — sending BadImplementation for major=%u minor=%u seq=%u\n",
              (unsigned)major, (unsigned)minor, (unsigned)seq);
      ctx_.transport().sendErrorCore(x11::error::BadImplementation, seq, 0, major);
    }

    return 1;
  } catch (const std::exception& ex) {
#ifndef NDEBUG
    ctx_.tracef("[XProtoServer] dispatch EXCEPTION major=%u minor=%u seq=%u remain=%zu: %s\n",
                (unsigned)major, (unsigned)minor, (unsigned)seq, remain, ex.what());
#endif
    // Safety net: if reply-bearing and no reply sent before exception, send error.
    if (ctx_.hasClient() && isReplyBearingCore(major) && !ctx_.transport().wasReplySent()) {
      fprintf(stderr,
              "[DISPATCH] EXCEPTION — sending BadImplementation for major=%u seq=%u\n",
              (unsigned)major, (unsigned)seq);
      ctx_.transport().sendErrorCore(x11::error::BadImplementation, seq, 0, major);
    }
    return 1;
  } catch (...) {
#ifndef NDEBUG
    ctx_.tracef("[XProtoServer] dispatch UNKNOWN EXCEPTION major=%u minor=%u seq=%u remain=%zu\n",
                (unsigned)major, (unsigned)minor, (unsigned)seq, remain);
#endif
    // Safety net: if reply-bearing and no reply sent before exception, send error.
    if (ctx_.hasClient() && isReplyBearingCore(major) && !ctx_.transport().wasReplySent()) {
      fprintf(stderr,
              "[DISPATCH] UNKNOWN EXCEPTION — sending BadImplementation for major=%u seq=%u\n",
              (unsigned)major, (unsigned)seq);
      ctx_.transport().sendErrorCore(x11::error::BadImplementation, seq, 0, major);
    }
    return 1;
  }
}


void XProtoServer::registerMajor(uint8_t major, HandlerFn fn, void* user) {
#ifndef NDEBUG
  if (table_[major].fn) {
    fprintf(stderr,
            "[REG] WARNING: major=%u already registered (old user=%p) -> overwriting with user=%p\n",
            (unsigned)major, table_[major].user, user);
  }
#endif
  table_[major].fn = fn;
  table_[major].user = user;
}


void XProtoServer::setWindowLookup(WindowLookupFn fn, void* user) {
  injected_lookup_ = fn;
  injected_lookup_user_ = user;
  ctx_.setWindowLookup(&XProtoServer::lookupWindowTrampoline, this);
}

void XProtoServer::queueNotify(uint32_t wid, bool wantConfigure, bool wantExpose) {
  if (!ctx_.hasClient()) return;
  ctx_.transport().queueNotify(wid, wantConfigure, wantExpose);
}

void XProtoServer::flushNotifyQueue() {
  if (!ctx_.hasClient()) return;
  ctx_.transport().flushNotifyQueue();
}

void XProtoServer::setTestWindow(const WindowView& w) {
  if (!impl_) return;
  impl_->testWindows[w.xid] = w;
}

void XProtoServer::clearTestWindows() {
  if (!impl_) return;
  impl_->testWindows.clear();
}

void XProtoServer::flushPendingMaps() {
  if (pending_maps_.empty()) return;
  for (uint32_t wid : pending_maps_) {
    WindowView vw{};
    if (!windows_.snapshot(wid, vw)) continue;

    // ── SubstructureRedirect emulation: authoritative size override ─────
    // After draining all buffered client data, the window's geometry may
    // STILL be tiny (e.g., Java AWT undoes its own ConfigureWindow AND
    // WM_NORMAL_HINTS before MapWindow).
    //
    // Resolution order:
    //   1. WM_NORMAL_HINTS (ICCCM authoritative — real WMs read at MapRequest)
    //   2. Peak pre-map size (largest ConfigureWindow seen before map)
    //   3. Give up: map at current tiny size
    if (vw.w < 50 || vw.h < 50) {
      int32_t desired_w = 0, desired_h = 0;

      // Try WM_NORMAL_HINTS first
      x11::PropertyTable::Prop hints_prop;
      if (x11::PropertyTable::instance().get(wid, x11::atom::kWM_NORMAL_HINTS, hints_prop) &&
          hints_prop.format == 32 && hints_prop.data.size() >= 5 * 4) {
        const uint32_t* d32 = reinterpret_cast<const uint32_t*>(hints_prop.data.data());
        const uint32_t flags = d32[0];
        if ((flags & 0x08) && hints_prop.data.size() >= 5 * 4) { // PSize
          desired_w = (int32_t)d32[3];
          desired_h = (int32_t)d32[4];
        }
        if ((flags & 0x10) && hints_prop.data.size() >= 7 * 4) { // PMinSize
          int32_t mw = (int32_t)d32[5], mh = (int32_t)d32[6];
          if (mw > desired_w) desired_w = mw;
          if (mh > desired_h) desired_h = mh;
        }
        if ((flags & 0x100) && hints_prop.data.size() >= 17 * 4) { // PBaseSize
          int32_t bw = (int32_t)d32[15], bh = (int32_t)d32[16];
          if (bw > desired_w) desired_w = bw;
          if (bh > desired_h) desired_h = bh;
        }
      }

      // If WM_NORMAL_HINTS didn't help (also tiny or missing), try peak size
      if (desired_w < 50 || desired_h < 50) {
        uint16_t pw = 0, ph = 0;
        if (getPeakSize(wid, pw, ph) && pw >= 50 && ph >= 50) {
          desired_w = (int32_t)pw;
          desired_h = (int32_t)ph;
#ifndef NDEBUG
          fprintf(stderr, "[FLUSH_MAP_PEAK] wid=0x%08X: geom %ux%u → peak %ux%u\n",
                  (unsigned)wid, (unsigned)vw.w, (unsigned)vw.h,
                  (unsigned)pw, (unsigned)ph);
#endif
        }
      }

      if (desired_w >= 50 && desired_h >= 50 &&
          ((int32_t)vw.w != desired_w || (int32_t)vw.h != desired_h)) {
        uint16_t nw = (uint16_t)std::min(desired_w, (int32_t)65535);
        uint16_t nh = (uint16_t)std::min(desired_h, (int32_t)65535);
#ifndef NDEBUG
        fprintf(stderr, "[FLUSH_MAP_RESIZE] wid=0x%08X: geom %ux%u → %ux%u\n",
                (unsigned)wid, (unsigned)vw.w, (unsigned)vw.h,
                (unsigned)nw, (unsigned)nh);
#endif
        // Center on primary monitor if no explicit position
        auto layout = x11::getScreenLayout();
        const x11::MonitorInfo* primary = nullptr;
        for (auto& m : layout.monitors) {
          if (m.is_primary) { primary = &m; break; }
        }
        if (!primary && !layout.monitors.empty())
          primary = &layout.monitors[0];
        if (primary) {
          int32_t cx = primary->x + ((int32_t)primary->w - (int32_t)nw) / 2;
          int32_t cy = primary->y + ((int32_t)primary->h - (int32_t)nh) / 2;
          if (cx < primary->x) cx = primary->x;
          if (cy < primary->y) cy = primary->y;
          windows_.setGeometry(wid, (int16_t)cx, (int16_t)cy, nw, nh);
        } else {
          windows_.setGeometry(wid, vw.x, vw.y, nw, nh);
        }
        // Re-snapshot after resize
        windows_.snapshot(wid, vw);
      }
    }

    // Clear peak size now that window is being mapped
    clearPeakSize(wid);

#ifndef NDEBUG
    fprintf(stderr, "[FLUSH_MAP] wid=0x%08X geom=%ux%u — pushing deferred map\n",
            (unsigned)wid, (unsigned)vw.w, (unsigned)vw.h);
#endif
    x11_ui_push_map(wid);
    x11_ui_push_resize(wid, (int32_t)vw.w, (int32_t)vw.h);
    if (vw.override_redirect) {
      x11_ui_push_move(wid, (int32_t)vw.x, (int32_t)vw.y);
    }

    // _NET_FRAME_EXTENTS
    {
      uint8_t extents[16] = {0};
      if (!vw.override_redirect) {
        x11::wire::wr32_le(extents + 0, 0);   // left
        x11::wire::wr32_le(extents + 4, 0);   // right
        x11::wire::wr32_le(extents + 8, 28);  // top (title bar)
        x11::wire::wr32_le(extents + 12, 0);  // bottom
      }
      PropertyTable::instance().setReplace(wid, x11::atom::k_NET_FRAME_EXTENTS,
                                           x11::atom::kATOM, 32, extents, 16);
    }
  }
  pending_maps_.clear();
}

void XProtoServer::pendingMapTrampoline(uint32_t wid, void* user) {
  if (user) static_cast<XProtoServer*>(user)->addPendingMap(wid);
}

void XProtoServer::peakSizeTrampoline(uint32_t wid, uint16_t w, uint16_t h, void* user) {
  if (user) static_cast<XProtoServer*>(user)->notePeakSize(wid, w, h);
}

void XProtoServer::notePeakSize(uint32_t wid, uint16_t w, uint16_t h) {
  uint32_t area = (uint32_t)w * (uint32_t)h;
  auto it = peak_sizes_.find(wid);
  if (it == peak_sizes_.end()) {
    peak_sizes_[wid] = PeakSize{w, h};
  } else {
    uint32_t old_area = (uint32_t)it->second.w * (uint32_t)it->second.h;
    if (area > old_area) {
      it->second = PeakSize{w, h};
    }
  }
#ifndef NDEBUG
  auto& ps = peak_sizes_[wid];
  fprintf(stderr, "[PEAK_SIZE] wid=0x%08X configure=%ux%u peak=%ux%u\n",
          (unsigned)wid, (unsigned)w, (unsigned)h,
          (unsigned)ps.w, (unsigned)ps.h);
#endif
}

bool XProtoServer::getPeakSize(uint32_t wid, uint16_t& outW, uint16_t& outH) const {
  auto it = peak_sizes_.find(wid);
  if (it == peak_sizes_.end()) return false;
  outW = it->second.w;
  outH = it->second.h;
  return true;
}

void XProtoServer::clearPeakSize(uint32_t wid) {
  peak_sizes_.erase(wid);
}

bool XProtoServer::lookupWindowTrampoline(uint32_t xid, WindowView* out, void* user) {
  if (!user || !out) return false;
  return static_cast<XProtoServer*>(user)->lookupWindow(xid, out);
}

bool XProtoServer::lookupWindow(uint32_t xid, WindowView* out) {
  if (!out) return false;

  if (injected_lookup_) {
    return injected_lookup_(xid, out, injected_lookup_user_);
  }

  if (!impl_) return false;
  auto it = impl_->testWindows.find(xid);
  if (it == impl_->testWindows.end()) return false;
  *out = it->second;
  return true;
}

void XProtoServer::updateSurface(uint32_t xid, const SurfaceDesc& s) {
  if (xid == 0) return;

  const uint32_t host = ctx_.windows().topLevelAncestorOf(xid);
  const uint32_t key  = host ? host : xid;

#ifdef X11_TRACE_VERBOSE
  fprintf(stderr, "[UPDATE_SURFACE] xid=0x%08X -> host/key=0x%08X wh=%ux%u bpr=%u ptr=%p\n",
          (unsigned)xid, (unsigned)key,
          (unsigned)s.w, (unsigned)s.h, (unsigned)s.bytesPerRow, s.ptr);
#endif

  ctx_.surfaces().set(key, s);
  ctx_.windows().setPresentable(key, true);

  {
    x11::WindowView uv{};
    if (ctx_.windows().snapshot(key, uv)) {
      x11_shared_damage_union(key, 0, 0, (int32_t)uv.w, (int32_t)uv.h);
      x11_ui_push_damage(key, 0, 0, (int32_t)uv.w, (int32_t)uv.h);
    }
  }
}

void XProtoServer::clearSurface(uint32_t xid) {
  if (xid == 0) return;
  const uint32_t host = ctx_.windows().topLevelAncestorOf(xid);
  const uint32_t key  = host ? host : xid;

  ctx_.surfaces().clear(key);
  ctx_.windows().setPresentable(key, false);
  x11_shared_damage_clear(key);
}

} // namespace x11
