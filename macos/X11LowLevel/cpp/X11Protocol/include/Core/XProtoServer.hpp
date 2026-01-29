//
//  XProtoServer.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#pragma once
#include <cstdint>
#include <cstddef>
#include <array>
#include <memory>

// scaffold
#include "XProtoContext.hpp"
#include "XProtoTransport.hpp"
#include "XProtoRegistrar.hpp"
#include "ReplyWriter.hpp"
#include "ByteReader.hpp"
#include "WindowTable.hpp"
#include "PixmapTable.hpp"

namespace x11 {

  class EventOps;

// Minimal "owning" object that wires Context + Transport + EventOps.
//
// Today this exists mainly so you can:
//  - move queue_notify / flush_notify_queue into Transport
//  - make those behaviors unit-testable without touching the C globals
//  - later hang opcode-family handlers off of this server instance
//
// Window lookup:
//  - In production, you will inject a C callback that snapshots x11_win_t into WindowView.
//  - In tests, you can populate a small in-memory map via setTestWindow().
class XProtoServer : public XProtoRegistrar {
public:
  using WindowLookupFn = x11::WindowLookupFn;

  XProtoServer();
  ~XProtoServer();
  
  // Inject the production lookup (C-side snapshot) and an opaque user pointer.
  void setWindowLookup(WindowLookupFn fn, void* user);

  // Dispatch one decoded X11 request body into C++ ops.
  // `seq` is the server-side request sequence number you are already tracking.
  void registerMajor(uint8_t major, HandlerFn fn, void* user) override;
  int  dispatch(uint8_t major, uint8_t minor, uint16_t seq,
                const uint8_t* payload, std::size_t remain);

  // Transport plumbing.
  void attachClientFd(int fd);
  void setXprotoThreadSelf();
  void noteLastSeq(uint16_t seq);

  // ---- Notify queue API (what you previously had as C globals) ----
  void queueNotify(uint32_t wid, bool wantConfigure, bool wantExpose);
  void flushNotifyQueue();

  // Accessors for tests / future opcode modules.
  XProtoContext& ctx() { return ctx_; }
  XProtoTransport& transport() { return transport_; }
  EventOps& eventOps() { return *eventOps_; }

  // ---- Test support ----
  // Populate a fake window snapshot map used when no WindowLookupFn is installed.
  void setTestWindow(const WindowView& w);
  void clearTestWindows();

private:
  // Context callback used when you call setWindowLookup().
  static bool lookupWindowTrampoline(uint32_t xid, WindowView* out, void* user);

  // Default lookup behavior:
  //  - if an injected lookup exists, use it
  //  - otherwise fall back to the test map
  bool lookupWindow(uint32_t xid, WindowView* out);

private:
  WindowTable windows_;
  PixmapTable pixmapTable_;
  XProtoContext   ctx_;

  // opcode-family modules
  std::unique_ptr<x11::EventOps> eventOps_;
  
  XProtoTransport transport_;
  ReplyWriter     reply_;
  
  struct Entry {
    HandlerFn fn = nullptr;
    void* user = nullptr;
  };
  std::array<Entry, 256> table_{};

  // Optional injected production lookup.
  WindowLookupFn injected_lookup_ = nullptr;
  void* injected_lookup_user_ = nullptr;

  // Test-only backing store (used when injected_lookup_ == nullptr).
  // Kept in the .cpp to avoid exposing <unordered_map> in public headers.
  struct Impl;
  std::unique_ptr<Impl> impl_;
  
};

} // namespace x11

