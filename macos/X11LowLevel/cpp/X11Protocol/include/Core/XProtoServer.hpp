//
//  XProtoServer.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/19/26.
//
//  Persistent server instance — owns all server-wide state (windows, pixmaps,
//  fonts, cursors, surfaces, grabs, input, dispatch table).
//  Per-client state (transport, reply writer) lives in XClient.
//

#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Core/XProtoContext.hpp>
#include <Core/XProtoRegistrar.hpp>
#include <Utils/ByteReader.hpp>
#include <Core/WindowTable.hpp>
#include <Core/PixmapTable.hpp>
#include <Core/FontTable.hpp>
#include <Core/CursorTable.hpp>
#include <Core/DrawableSurfaceRegistry.hpp>
#include <Core/GrabTable.hpp>
#include <Core/InputState.hpp>
#include <Core/HostCommandQueue.hpp>
#include <UI/UICommandQueue.hpp>

namespace x11 {

  class EventOps;

// Persistent server — created once on first client connection, survives across
// sessions.  Each client session creates an XClient and wires it into the
// context via ctx().setClient(&client).
class XProtoServer : public XProtoRegistrar {
public:
  using WindowLookupFn = x11::WindowLookupFn;

  XProtoServer();
  ~XProtoServer();

  // Inject the production lookup (C-side snapshot) and an opaque user pointer.
  void setWindowLookup(WindowLookupFn fn, void* user);

  // Dispatch one decoded X11 request body into C++ ops.
  void registerMajor(uint8_t major, HandlerFn fn, void* user) override;
  int  dispatch(uint8_t major, uint8_t minor, uint16_t seq,
                const uint8_t* payload, std::size_t remain);

  // ---- Drawable surfaces ----
  void updateSurface(uint32_t xid, const SurfaceDesc& s);
  void clearSurface(uint32_t xid);

  // ---- Notify queue (convenience, delegates to ctx().transport()) ----
  void queueNotify(uint32_t wid, bool wantConfigure, bool wantExpose);
  void flushNotifyQueue();

  // ---- Accessors ----
  XProtoContext& ctx() { return ctx_; }
  const XProtoContext& ctx() const { return ctx_; }
  EventOps& eventOps() { return *eventOps_; }

  // Server-wide tables
  WindowTable& windows() { return windows_; }
  const WindowTable& windows() const { return windows_; }
  FontTable& fonts() { return fonts_; }
  const FontTable& fonts() const { return fonts_; }
  GrabTable& grabs() { return grabs_; }
  InputState& input() { return input_; }
  HostCommandQueue& hostCmds() { return hostCmds_; }

  // ---- SubstructureRedirect emulation ----
  // Tiny root children defer their map UI push until all buffered client
  // requests have been processed (emulating quartz-wm MapRequest delay).
  void addPendingMap(uint32_t wid) { pending_maps_.push_back(wid); }
  bool hasPendingMaps() const { return !pending_maps_.empty(); }
  // Flush all pending maps: push x11_ui_push_map + resize for each.
  void flushPendingMaps();

  // Peak pre-map size: track the largest ConfigureWindow size seen on each
  // unmapped root child.  Used by flushPendingMaps when both geometry and
  // WM_NORMAL_HINTS are tiny (client undid its own resize before map).
  void notePeakSize(uint32_t wid, uint16_t w, uint16_t h);
  bool getPeakSize(uint32_t wid, uint16_t& outW, uint16_t& outH) const;
  void clearPeakSize(uint32_t wid);

  // Pending post-map ConfigureNotify: set by flushPendingMaps when it
  // overrides the client's last ConfigureWindow (shrunk-below-peak rescue).
  // SetPresentable handler drains this — it runs with a valid client
  // transport and queues the ConfigureNotify so the client relayouts.
  void markNeedsPostMapConfigureNotify(uint32_t wid);
  bool takeNeedsPostMapConfigureNotify(uint32_t wid);

  // ---- Test support ----
  void setTestWindow(const WindowView& w);
  void clearTestWindows();

private:
  static bool lookupWindowTrampoline(uint32_t xid, WindowView* out, void* user);
  bool lookupWindow(uint32_t xid, WindowView* out);
  static void pendingMapTrampoline(uint32_t wid, void* user);
  static void peakSizeTrampoline(uint32_t wid, uint16_t w, uint16_t h, void* user);
  static bool getPeakSizeTrampoline(uint32_t wid, uint16_t* outW, uint16_t* outH, void* user);

private:
  // Server-wide state (persists across client sessions)
  WindowTable windows_;
  PixmapTable pixmapTable_;
  FontTable fonts_;
  x11::CursorTable cursors_;
  DrawableSurfaceRegistry surfaces_;
  GrabTable grabs_;
  InputState input_;
  UICommandQueue ui_;
  HostCommandQueue hostCmds_;

  // Protocol context (wiring hub — per-client pointers set via setClient)
  XProtoContext ctx_;
  std::unique_ptr<x11::EventOps> eventOps_;

  // Opcode dispatch table
  struct Entry {
    HandlerFn fn = nullptr;
    void* user = nullptr;
  };
  std::array<Entry, 256> table_{};

  // SubstructureRedirect emulation: pending maps.
  std::vector<uint32_t> pending_maps_;

  // Peak pre-map size: largest ConfigureWindow size seen per unmapped root child.
  struct PeakSize { uint16_t w = 0; uint16_t h = 0; };
  std::unordered_map<uint32_t, PeakSize> peak_sizes_;

  // Windows whose size was rescued by flushPendingMaps (shrunk-below-peak).
  // SetPresentable must emit a ConfigureNotify so the client relayouts.
  std::unordered_set<uint32_t> needs_post_map_configure_notify_;

  // Optional injected production lookup.
  WindowLookupFn injected_lookup_ = nullptr;
  void* injected_lookup_user_ = nullptr;

  // Test-only backing store (used when injected_lookup_ == nullptr).
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace x11
