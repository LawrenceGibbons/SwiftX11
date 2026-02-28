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

  // ---- Test support ----
  void setTestWindow(const WindowView& w);
  void clearTestWindows();

private:
  static bool lookupWindowTrampoline(uint32_t xid, WindowView* out, void* user);
  bool lookupWindow(uint32_t xid, WindowView* out);

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

  // Optional injected production lookup.
  WindowLookupFn injected_lookup_ = nullptr;
  void* injected_lookup_user_ = nullptr;

  // Test-only backing store (used when injected_lookup_ == nullptr).
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace x11
