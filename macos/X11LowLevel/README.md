#  Class diagram for the low level X11 protocol

[XProtoServer thread]
  XProtoServer
    ├─ Connection(fd, seq)
    ├─ RequestParser
    ├─ ReplyWriter (socket writes ONLY here)
    ├─ XProtoDispatcher
    └─ XProtoState
         ├─ AtomTable
         ├─ PropertyStore
         ├─ WindowStore (Window + Framebuffer)
         ├─ PixmapStore (Pixmap bits/pixels)
         └─ GCStore

  -> pushes Request into ServerRequestQueue  (thread-safe)

[Shim/Server thread]
  ShimServerLoop
    ├─ drains ServerRequestQueue
    ├─ updates BackendTruth (alive/mapped/closing/inflight)
    ├─ emits BackendEventQueue (x11_event_t)
    └─ pushes NotifyQueue entries (ConfigureNotify/Expose)

[Back to XProtoServer thread]
  XProtoServer flushes NotifyQueue and writes events on socket


What each file is in XProtoServer thread responsible for

Thread + lifecycle
  •  XProtoServer.{h,cpp}
  •  Owns the listener socket (lfd), accept loop thread, start/stop.
  •  Accepts connections and creates one XProtoConnection per client socket.
  •  Owns global server-wide shared objects used by the xproto thread(s) (atoms table, etc.) as needed.

Per-client connection loop (your current drain_requests)
  •  XProtoConnection.{h,cpp}
  •  Handles the X11 handshake (SetupRequest → SetupSuccess/Failed).
  •  Runs the request loop: read 4-byte header, read payload, update sequence, dispatch.
  •  Stores connection-specific state: fd, seq, clientId/owner_fd, last-seq for synthetic events, etc.
  •  Calls notifyQueue.flush(fd, seq) in safe places.

Dispatch layer
  •  XProtoDispatcher.{h,cpp}
  •  Single entry point: dispatch(major, minor, payload, len, seq, connection, state)
  •  No socket I/O directly besides replying via helpers (or through connection.io()).
  •  Routes to opcode-family handlers later (we’ll add those in the next step).

Socket I/O (recv/send)
  •  XProtoIO.{h,cpp}
  •  recvExact(), sendAll(), timeouts, EINTR/EAGAIN handling, SO_NOSIGPIPE handling.
  •  “must be xproto-thread” debug guard (your dbg_require_xproto_thread).
  •  Nothing protocol-specific.

Wire helpers (endianness + packing)
  •  XProtoWire.{h,cpp}
  •  rd16/rd32, wr16_le/wr32_le, plus small buffer builders for replies/events.
  •  Optional “struct view” helpers to decode request bodies safely.

SetupSuccess/Failed builder
  •  XProtoSetup.{h,cpp}
  •  Builds SetupSuccess response (your big x11_send_setup_success_minimal_little_endian).
  •  Central place to define server-advertised parameters:
  •  bitmapBitOrder, bitmapScanlinePad, supported pixmap formats, root visual, etc.

Atom table
  •  XProtoAtoms.{h,cpp}
  •  Predefined atoms init, dynamic atom allocation starting at 69.
  •  intern(name, onlyIfExists), name(atomId).

Properties store
  •  XProtoProperties.{h,cpp}
  •  GetProperty, ChangeProperty, property storage, delete, append/prepend.
  •  Keeps the “WM_NAME / _NET_WM_NAME → set_title” bridge helper here (or in dispatcher helper).

Cross-thread safe “send events later” queue
  •  XProtoNotifyQueue.{h,cpp}
  •  Thread-safe queue for Expose / ConfigureNotify requests coming from non-xproto threads.
  •  queue(wid, want_configure, want_expose)
  •  flush(connection_fd, seq) — must run only on xproto thread.

Debug / tracing knobs
  •  XProtoTrace.{h,cpp}
  •  Compile-time knobs like your SWIFTX11_TRACE, “dump request header”, “reply length check”.
  •  Centralized so you don’t sprinkle #ifndef NDEBUG everywhere.

Core xproto-owned state (your big globals)
  •  XProtoState.{h,cpp}
  •  Owns the xproto-side window table + framebuffers + pixmaps + GCs + properties.
  •  This becomes the “model” that opcode modules mutate.
  •  Important: this is not the backend/shim truth; it’s the xproto-side truth for drawables.

Shared structs / enums
  •  XProtoTypes.h
  •  struct X11Window, struct Pixmap, struct GC, struct Framebuffer, etc.
  •  enum class MajorOpcode : uint8_t etc.

Notes / dev guide
  •  README_XPROTO.md
  •  Documents thread model, invariants, what can write to socket, where to add new opcodes.
