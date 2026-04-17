//
//  XProtoDaemon.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/3/26.
//

#include "Transport/XProtoDaemon.hpp"
#include "Core/IncrTransfer.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <algorithm>
#include <vector>

#include "XProtoServerBridge.h"
#include "Core/XProtoServer.hpp"
#include "Core/XProtoModules.hpp"
#include "Core/XProtoContext.hpp"
#include "Core/XClient.hpp"
#include "Core/HostCommandQueue.hpp"
#include "Core/WindowTable.hpp"
#include "Core/WindowView.hpp"
#include "Core/GrabTable.hpp"
#include "Core/InputState.hpp"
#include "Core/PropertyTable.hpp"
#include "Core/ClipboardAtoms.hpp"
#include "Core/ScreenLayout.hpp"
#include "Core/XConstants.hpp"
#include "Core/X11ExtOpcodes.hpp"
#include "Core/timestamp.hpp"
#include "Ops/EventOps.hpp"
#include "Transport/X11Setup.hpp"
#include "Utils/WireLE.hpp"
#include "Utils/MachTime.hpp"

extern "C" void x11_cpp_notify_init(void* ctx_ptr, void* event_ops_ptr, void* queue_ptr);
extern "C" void x11_cpp_notify_shutdown(void);
extern "C" void x11_ui_push_destroy(uint32_t xid);
extern "C" void x11_ui_push_log(int level, const char* message);
extern "C" int x11_get_wire_trace(void);

static std::atomic<uint32_t> g_nextClientSlot{1}; // 0 reserved


namespace x11 {

static int make_listen_socket(int display, const char* bindAddr) {
  const int port = 6000 + display;

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = ::inet_addr(bindAddr);

  if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { ::close(fd); return -1; }
  if (::listen(fd, 16) < 0) { ::close(fd); return -1; }

  return fd;
}

/// Create a Unix domain socket listener at /tmp/.X11-unix/X{display}.
static int make_listen_socket_unix(int display, std::string& outPath) {
  // Ensure /tmp/.X11-unix/ directory exists (sticky bit, world-writable)
  ::mkdir("/tmp/.X11-unix", 01777);

  char path[108];
  ::snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", display);

  // Remove stale socket file
  ::unlink(path);

  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  ::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }

  // Allow Docker containers (different UID) to connect
  ::chmod(path, 0777);

  if (::listen(fd, 16) < 0) {
    ::close(fd);
    ::unlink(path);
    return -1;
  }

  outPath = path;
  return fd;
}

static bool recv_waitall(int fd, void* buf, size_t n) {
  uint8_t* p = (uint8_t*)buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::recv(fd, p + got, n - got, 0);
    if (r == 0) return false;
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    got += (size_t)r;
  }
  return true;
}

static bool skip_bytes(int fd, size_t n) {
  uint8_t tmp[256];
  while (n) {
    size_t want = (n > sizeof(tmp)) ? sizeof(tmp) : n;
    if (!recv_waitall(fd, tmp, want)) return false;
    n -= want;
  }
  return true;
}

static void set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


// ---------- lifecycle ----------
XProtoDaemon::XProtoDaemon() = default;
XProtoDaemon::~XProtoDaemon() {
  stop();
  delete modules_; modules_ = nullptr;
  delete server_;  server_  = nullptr;
}

void XProtoDaemon::ensureServer() {
  if (server_) return;
  server_  = new x11::XProtoServer();
  modules_ = new x11::XProtoModules(*server_);
}

bool XProtoDaemon::start(int display, bool enableTCP, bool enableUnix,
                         const char* tcpBindAddr) {
  if (running_.load(std::memory_order_acquire)) return true;

  if (!enableTCP && !enableUnix) {
    TS_FPRINTF("[X11] ERROR: both TCP and Unix socket disabled — no listeners!\n");
    return false;
  }

  stop_.store(false, std::memory_order_release);
  // Copy bindAddr since the pointer may not outlive the caller
  std::string bindAddrStr = tcpBindAddr ? tcpBindAddr : "0.0.0.0";
  th_ = std::thread([this, display, enableTCP, enableUnix, bindAddrStr] {
    runListener(display, enableTCP, enableUnix, bindAddrStr.c_str());
  });

  return true;
}

void XProtoDaemon::stop() {
  stop_.store(true, std::memory_order_release);

  // Close all listen fds to unblock poll
  for (int fd : listen_fds_) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
  listen_fds_.clear();

  // Unlink Unix socket if we created one
  if (!unix_socket_path_.empty()) {
    ::unlink(unix_socket_path_.c_str());
    unix_socket_path_.clear();
  }

  if (th_.joinable()) th_.join();
}

ClientSession* XProtoDaemon::findClient(int fd) {
  auto it = clients_.find(fd);
  return (it != clients_.end()) ? &it->second : nullptr;
}

bool XProtoDaemon::sendEventCrossClient(uint32_t targetWid, const uint8_t ev[32]) {
  if (!server_ || !ev) return false;
  const x11::WindowView* wv = server_->ctx().window(targetWid);
  if (!wv || wv->owner_fd <= 0) return false;

  ClientSession* cs = findClient(wv->owner_fd);
  if (!cs || !cs->client) return false;

  // Send directly on the target client's transport WITHOUT activating it.
  // activateClient/deactivateClient would clobber the currently-active
  // client context, causing a null transport crash when the caller resumes.
  //
  // CRITICAL: restamp bytes[2:3] (sequence) with the TARGET transport's
  // lastSeq().  The event was built with the SOURCE client's sequence,
  // but the target client's max_wire_seq_ monotonic floor would be
  // poisoned by a foreign (much larger/smaller) sequence number,
  // corrupting all subsequent replies and causing XCB desync crashes.
  uint8_t fixed[32];
  std::memcpy(fixed, ev, 32);
  uint16_t targetSeq = cs->client->transport().lastSeq();
  fixed[2] = static_cast<uint8_t>(targetSeq & 0xFF);
  fixed[3] = static_cast<uint8_t>((targetSeq >> 8) & 0xFF);
  return cs->client->transport().sendAll(fixed, 32);
}

bool XProtoDaemon::sendEventCrossClientVariable(uint32_t targetWid, const uint8_t* ev, size_t len) {
  if (!server_ || !ev || len < 32) return false;
  const x11::WindowView* wv = server_->ctx().window(targetWid);
  if (!wv || wv->owner_fd <= 0) return false;

  ClientSession* cs = findClient(wv->owner_fd);
  if (!cs || !cs->client) return false;

  // Restamp sequence for target transport (same rationale as above).
  std::vector<uint8_t> fixed(ev, ev + len);
  uint16_t targetSeq = cs->client->transport().lastSeq();
  fixed[2] = static_cast<uint8_t>(targetSeq & 0xFF);
  fixed[3] = static_cast<uint8_t>((targetSeq >> 8) & 0xFF);
  return cs->client->transport().sendAll(fixed.data(), fixed.size());
}


// ---------- poll loop ----------

void XProtoDaemon::runListener(int display, bool enableTCP, bool enableUnix,
                               const char* tcpBindAddr) {
  running_.store(true, std::memory_order_release);

  // Create listen sockets based on flags
  if (enableTCP) {
    int tcpFd = make_listen_socket(display, tcpBindAddr);
    if (tcpFd >= 0) {
      listen_fds_.push_back(tcpFd);
      TS_FPRINTF("[X11] TCP listener on %s:%d (fd=%d)\n",
              tcpBindAddr, 6000 + display, tcpFd);
    } else {
      TS_FPRINTF("[X11] WARNING: failed to create TCP listener on %s:%d\n",
              tcpBindAddr, 6000 + display);
    }
  }

  if (enableUnix) {
    std::string unixPath;
    int unixFd = make_listen_socket_unix(display, unixPath);
    if (unixFd >= 0) {
      listen_fds_.push_back(unixFd);
      unix_socket_path_ = unixPath;
      TS_FPRINTF("[X11] Unix socket listener on %s (fd=%d)\n",
              unixPath.c_str(), unixFd);
    } else {
      TS_FPRINTF("[X11] WARNING: failed to create Unix socket listener\n");
    }
  }

  if (listen_fds_.empty()) {
    TS_FPRINTF("[X11] ERROR: no listeners created — aborting\n");
    running_.store(false, std::memory_order_release);
    return;
  }

  ensureServer();

  while (!stop_.load(std::memory_order_acquire)) {
    // Build pollfd array: listen sockets first, then client fds
    std::vector<struct pollfd> fds;
    for (int lfd : listen_fds_)
      fds.push_back({lfd, POLLIN, 0});

    const size_t numListeners = listen_fds_.size();

    std::vector<int> clientFds;
    for (auto& [fd, _] : clients_) {
      fds.push_back({fd, POLLIN, 0});
      clientFds.push_back(fd);
    }

    int nready = ::poll(fds.data(), (nfds_t)fds.size(), 50 /*ms*/);
    if (nready < 0) {
      if (errno == EINTR) continue;
      break;
    }

    // Check ALL listen sockets for new connections
    for (size_t i = 0; i < numListeners; i++) {
      if (fds[i].revents & POLLIN) {
        acceptClient(listen_fds_[i]);
      }
    }

    // Drain host commands FIRST so client requests (e.g. xeyes' QueryPointer)
    // see the freshest InputState (pointer position updated by PointerMove).
    drainHostCommands();

    // Check client sockets — drain ALL buffered data per client.
    // This emulates SubstructureRedirect: all requests in a TCP segment
    // (CreateWindow, ChangeProperty, ConfigureWindow, MapWindow) are
    // processed before the map is pushed to the UI.
    std::vector<int> toRemove;
    for (size_t i = 0; i < clientFds.size(); i++) {
      int cfd = clientFds[i];
      short rev = fds[i + numListeners].revents;

      // When POLLIN and POLLHUP arrive together (revents=0x11), the client
      // sent data and then closed its end.  We MUST drain and process any
      // remaining requests before disconnecting — they may be reply-bearing
      // (e.g. XTestGetVersion, XIQueryVersion on a probe connection).  If we
      // skip them, XCB never receives the expected reply and Vivado's probe
      // thread can dereference a NULL reply → signal 11.
      const bool wantDisconnect = (rev & (POLLHUP | POLLERR)) != 0;

      if (rev & POLLIN) {
        auto* cs = findClient(cfd);
        if (!cs) { if (wantDisconnect) toRemove.push_back(cfd); continue; }

        // Drain loop: process all complete requests in the socket buffer.
        for (;;) {
          DispatchResult dr = readAndDispatch(cfd, *cs);
          if (dr == DispatchResult::Error) {
            {
              char buf[128];
              snprintf(buf, sizeof(buf),
                       "[X11] readAndDispatch error fd=%d seq=%u\n",
                       cfd, (unsigned)cs->seq);
              x11_ui_push_log(1, buf);
            }
            toRemove.push_back(cfd);
            break;
          }
          if (dr == DispatchResult::Eof) {
            // Clean client-side close — not an error.
            cs->clean_disconnect = true;
            toRemove.push_back(cfd);
            break;
          }
          if (dr == DispatchResult::NeedMore) {
            break; // no more complete requests — done draining
          }
          // dr == Dispatched → try another request
        }
      }

      // Disconnect after draining (or immediately if no POLLIN data).
      if (wantDisconnect && std::find(toRemove.begin(), toRemove.end(), cfd) == toRemove.end()) {
        {
          char buf[128];
          snprintf(buf, sizeof(buf),
                   "[X11] poll disconnect fd=%d revents=0x%x (%s%s)\n",
                   cfd, (unsigned)rev,
                   (rev & POLLHUP) ? "HUP" : "",
                   (rev & POLLERR) ? "ERR" : "");
          x11_ui_push_log(1, buf);
        }
        toRemove.push_back(cfd);
      }
    }

    // After draining all client data, flush any deferred maps.
    // Pending maps were queued by MapWindow for tiny root children;
    // now all ConfigureWindow/ChangeProperty requests have been processed.
    flushPendingMaps();

    for (int fd : toRemove) {
      removeClient(fd);
    }
  }

  // Clean up all remaining clients
  std::vector<int> remaining;
  for (auto& [fd, _] : clients_) remaining.push_back(fd);
  for (int fd : remaining) removeClient(fd);

  running_.store(false, std::memory_order_release);
}


void XProtoDaemon::acceptClient(int listenFd) {
  int cfd = ::accept(listenFd, nullptr, nullptr);
  if (cfd < 0) return;

#if defined(SO_NOSIGPIPE)
  int one = 1;
  ::setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

  // ---- Setup handshake (blocking — tiny, happens once per client) ----
  uint8_t req[12];
  if (!recv_waitall(cfd, req, sizeof(req))) {
    { char buf[128]; snprintf(buf, sizeof(buf),
        "[X11] acceptClient fd=%d handshake recv failed errno=%d (%s)\n",
        cfd, errno, strerror(errno)); x11_ui_push_log(1, buf); }
    ::close(cfd);
    return;
  }

  const char byte_order = (char)req[0];
  if (byte_order != 'l') {
    { char buf[128]; snprintf(buf, sizeof(buf),
        "[X11] acceptClient fd=%d rejected: byte_order=0x%02X (not little-endian)\n",
        cfd, (unsigned)(uint8_t)byte_order); x11_ui_push_log(1, buf); }
    x11_send_setup_failed_le(cfd, "SwiftX11: only little-endian supported");
    ::close(cfd);
    return;
  }

  const uint16_t auth_proto_len = (uint16_t)(req[6] | ((uint16_t)req[7] << 8));
  const uint16_t auth_data_len  = (uint16_t)(req[8] | ((uint16_t)req[9] << 8));

  size_t skip = 0;
  skip += ((size_t)auth_proto_len + 3u) & ~3u;
  skip += ((size_t)auth_data_len  + 3u) & ~3u;
  if (skip && !skip_bytes(cfd, skip)) {
    { char buf[128]; snprintf(buf, sizeof(buf),
        "[X11] acceptClient fd=%d auth skip failed (%zu bytes) errno=%d\n",
        cfd, skip, errno); x11_ui_push_log(1, buf); }
    ::close(cfd);
    return;
  }

  // ---- Allocate client ID space ----
  uint32_t clientSlot = g_nextClientSlot.fetch_add(1, std::memory_order_relaxed);
  if (clientSlot == 0) clientSlot = g_nextClientSlot.fetch_add(1, std::memory_order_relaxed);

  const uint32_t rid_mask = 0x00FFFFFFu;
  const uint32_t rid_base = (clientSlot & 0xFFu) << 24;

  x11_send_setup_success_minimal_little_endian(cfd, rid_base, rid_mask);

  {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "[X11] accept fd=%d slot=%u rid_base=0x%08X clients=%zu\n",
             cfd, (unsigned)clientSlot, (unsigned)rid_base,
             clients_.size() + 1);
    x11_ui_push_log(1, buf);
  }

  // Enlarge socket send buffer to reduce backpressure when the client
  // is slow to read (e.g., Java Swing during menu tracking, or Docker
  // VM network bridge bottleneck with large PutImage bursts).  The default
  // macOS SO_SNDBUF (~128KB) fills up quickly.  4MB absorbs typical
  // Vivado dialog rendering bursts over the Docker bridge.
  {
    int sndbuf = 4 * 1024 * 1024;
    ::setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  }

  // Switch to non-blocking for the poll loop
  set_nonblocking(cfd);

  // ---- Create client session ----
  ClientSession cs{};
  cs.client = new XClient(server_->ctx(), server_->eventOps(),
                          cfd, rid_base, rid_mask);

  // Initialize notify bridge on first client (points to server-wide ctx + evOps)
  if (!notifyBridgeInited_) {
    void* ctx_ptr = (void*)&server_->ctx();
    void* ev_ptr  = (void*)&server_->eventOps();
    x11_cpp_notify_init(ctx_ptr, ev_ptr, nullptr);
    notifyBridgeInited_ = true;
  }

  clients_.emplace(cfd, std::move(cs));
}


void XProtoDaemon::removeClient(int fd) {
  auto it = clients_.find(fd);
  if (it == clients_.end()) return;

  ClientSession& cs = it->second;

  // Clean disconnects log at verbose level and skip the ring-buffer dumps.
  // Error disconnects log at info level with full diagnostic dumps.
  const int log_level = cs.clean_disconnect ? 2 : 1;

  {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "[X11] %s fd=%d remaining=%zu seq=%u total_reqs=%u "
             "XI2[ver=%d dev=%d sel=%d list=%d]\n",
             cs.clean_disconnect ? "client closed" : "disconnect",
             fd, clients_.size() - 1,
             (unsigned)cs.seq, cs.total_requests,
             cs.sent_xi_query_version, cs.sent_xi_query_device,
             cs.sent_xi_select_events, cs.sent_list_input_devices);
    x11_ui_push_log(log_level, buf);
    fprintf(stderr, "%s", buf);
  }

  // Dump ring buffers only for error disconnects (not clean closes).
  if (!cs.clean_disconnect) {
    const char* hdr = "[X11] last dispatched requests (newest last):\n";
    x11_ui_push_log(1, hdr); fprintf(stderr, "%s", hdr);
    const int total = std::min(cs.history_idx, ClientSession::kHistorySize);
    const int start = cs.history_idx - total;
    for (int i = start; i < cs.history_idx; i++) {
      const auto& r = cs.history[i % ClientSession::kHistorySize];
      char buf[128];
      snprintf(buf, sizeof(buf),
               "  [%d] major=%u minor=%u seq=%u reply=%s\n",
               i - start, (unsigned)r.major, (unsigned)r.minor,
               (unsigned)r.seq, r.reply_sent ? "yes" : "no");
      x11_ui_push_log(1, buf); fprintf(stderr, "%s", buf);
    }

    // Dump last outgoing wire packets for crash diagnosis
    activateClient(cs);
    server_->ctx().transport().dumpWireHistory();
    deactivateClient();
  }

  // Activate this client for teardown
  activateClient(cs);

  // Erase windows owned by this client
  std::vector<uint32_t> owned = server_->ctx().windows().eraseOwnedBy(fd);
  for (uint32_t wid : owned) {
    x11_ui_push_destroy(wid);
  }

  // Remove grabs for destroyed windows
  server_->ctx().grabs().removeForWindows(owned);

  // Cancel any active INCR clipboard transfers for this client
  x11::IncrTransfer::instance().cancelForFd(fd);

  // Clear input state references to destroyed windows
  auto& input = server_->ctx().input();
  auto isOwned = [&](uint32_t xid) {
    return std::find(owned.begin(), owned.end(), xid) != owned.end();
  };
  if (input.drag_xid && isOwned(input.drag_xid)) {
    input.drag_xid = 0;
    input.buttons = 0;
  }
  if (input.focus_xid && isOwned(input.focus_xid)) input.focus_xid = 0;
  if (input.focus_host && isOwned(input.focus_host)) input.focus_host = 0;
  if (input.pointer_xid && isOwned(input.pointer_xid)) input.pointer_xid = 0;
  if (input.last_xid && isOwned(input.last_xid)) input.last_xid = 0;

  deactivateClient();

  // Destroy client and close socket
  delete cs.client;
  ::shutdown(fd, SHUT_RDWR);
  ::close(fd);

  clients_.erase(it);

  // If no clients remain, shut down notify bridge
  if (clients_.empty()) {
    x11_cpp_notify_shutdown();
    notifyBridgeInited_ = false;
  }
}


void XProtoDaemon::activateClient(ClientSession& cs) {
  server_->ctx().setClient(cs.client);
}

void XProtoDaemon::deactivateClient() {
  server_->ctx().clearClient();
}


DispatchResult XProtoDaemon::readAndDispatch(int fd, ClientSession& cs) {
  // Phase 0: read 4-byte request header
  if (cs.hdr_have < 4 && !cs.reading_ext_len) {
    ssize_t r = ::recv(fd, cs.hdr + cs.hdr_have, 4 - cs.hdr_have, 0);
    if (r == 0) return DispatchResult::Eof; // clean EOF — client closed socket
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return DispatchResult::NeedMore;
      return DispatchResult::Error;
    }
    cs.hdr_have += (size_t)r;
    if (cs.hdr_have < 4) return DispatchResult::NeedMore; // need more header bytes

    // Header complete — parse it
    const uint16_t len_words = (uint16_t)(cs.hdr[2] | ((uint16_t)cs.hdr[3] << 8));

    if (len_words == 0) {
      // BIG-REQUESTS: len_words==0 means next 4 bytes are the 32-bit length
      if (cs.client && cs.client->bigReqEnabled()) {
        cs.reading_ext_len = true;
        cs.ext_len_have = 0;
        // fall through to phase 0.5
      } else {
        return DispatchResult::Error; // protocol error — BIG-REQUESTS not enabled
      }
    } else {
      const size_t total = (size_t)len_words * 4u;
      if (total < 4u) return DispatchResult::Error;

      cs.buf_need = total - 4u;
      cs.buf_have = 0;

      if (cs.buf_need > 0) {
        cs.buf.resize(cs.buf_need);
        return DispatchResult::NeedMore; // wait for payload on next poll
      }

      // No payload — fall through to dispatch
    }
  }

  // Phase 0.5: read 4-byte extended length (BIG-REQUESTS)
  if (cs.reading_ext_len && cs.ext_len_have < 4) {
    ssize_t r = ::recv(fd, cs.ext_len + cs.ext_len_have, 4 - cs.ext_len_have, 0);
    if (r == 0) return DispatchResult::Eof;
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return DispatchResult::NeedMore;
      return DispatchResult::Error;
    }
    cs.ext_len_have += (size_t)r;
    if (cs.ext_len_have < 4) return DispatchResult::NeedMore; // need more bytes

    // Parse 32-bit extended length in words (little-endian)
    const uint32_t ext_words = (uint32_t)cs.ext_len[0]
                             | ((uint32_t)cs.ext_len[1] << 8)
                             | ((uint32_t)cs.ext_len[2] << 16)
                             | ((uint32_t)cs.ext_len[3] << 24);

    if (ext_words < 2) return DispatchResult::Error; // minimum is 2 words (8 bytes: 4 hdr + 4 ext_len)

    // Total bytes = ext_words * 4, already consumed 4 (hdr) + 4 (ext_len) = 8
    const size_t total = (size_t)ext_words * 4u;
    cs.buf_need = total - 8u;
    cs.buf_have = 0;
    cs.reading_ext_len = false;

    if (cs.buf_need > 0) {
      cs.buf.resize(cs.buf_need);
      return DispatchResult::NeedMore; // wait for payload on next poll
    }
    // No additional payload — fall through to dispatch
  }

  // Phase 1: read payload
  if (cs.buf_need > 0 && cs.buf_have < cs.buf_need) {
    ssize_t r = ::recv(fd, cs.buf.data() + cs.buf_have,
                       cs.buf_need - cs.buf_have, 0);
    if (r == 0) return DispatchResult::Eof;
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return DispatchResult::NeedMore;
      return DispatchResult::Error;
    }
    cs.buf_have += (size_t)r;
    if (cs.buf_have < cs.buf_need) return DispatchResult::NeedMore; // need more payload bytes
  }

  // Full request ready — dispatch
  const uint8_t major = cs.hdr[0];
  const uint8_t minor = cs.hdr[1];
  const uint8_t* payload = cs.buf_need > 0 ? cs.buf.data() : nullptr;
  const size_t remain = cs.buf_need;

  cs.seq = (uint16_t)(cs.seq + 1);

  // Live wire trace: log every incoming request to stderr when enabled.
  {
    if (x11_get_wire_trace()) {
      TS_FPRINTF("[WIRE] fd=%d REQ major=%u minor=%u seq=%u len=%zu\n",
              fd, (unsigned)major, (unsigned)minor, (unsigned)cs.seq, remain);
    }
  }

  activateClient(cs);
  server_->ctx().transport().noteLastSeq(cs.seq);
  (void)server_->dispatch(major, minor, cs.seq, payload, remain);

  // Flush pending notifies for this client only (NOT host commands —
  // those are drained separately in drainHostCommands with correct
  // per-client activation).
  server_->flushNotifyQueue();

  // Record in ring buffer for crash diagnosis
  cs.recordDispatch(major, minor, cs.seq,
                    server_->ctx().transport().wasReplySent());

  // Track XI2 requests for disconnect diagnostics
  if (major == ext::kXInput2) {
    if (minor == 47) cs.sent_xi_query_version = true;
    if (minor == 48) cs.sent_xi_query_device  = true;
    if (minor == 46) cs.sent_xi_select_events = true;
    if (minor == 2)  cs.sent_list_input_devices = true;
  }

  deactivateClient();

  // Reset for next request
  cs.hdr_have = 0;
  cs.buf_need = 0;
  cs.buf_have = 0;
  cs.reading_ext_len = false;
  cs.ext_len_have = 0;

  return DispatchResult::Dispatched;
}

void XProtoDaemon::flushPendingMaps() {
  if (server_) server_->flushPendingMaps();
}


// Check if a window's WM_PROTOCOLS property includes WM_DELETE_WINDOW.
static bool windowSupportsDeleteProtocol(uint32_t xid) {
  PropertyTable::Prop prop{};
  if (!PropertyTable::instance().get(xid, atom::kWM_PROTOCOLS, prop))
    return false;
  if (prop.format != 32) return false;

  // WM_PROTOCOLS is a list of CARD32 atoms
  const size_t count = prop.data.size() / 4;
  for (size_t i = 0; i < count; i++) {
    uint32_t a = wire::rd32_le(prop.data.data() + i * 4);
    if (a == atom::kWM_DELETE_WINDOW)
      return true;
  }
  return false;
}

// Send a WM_DELETE_WINDOW ClientMessage to a window.
static void sendDeleteWindowMessage(XProtoContext& ctx, uint32_t xid) {
  uint8_t ev[32] = {0};
  ev[0] = 33;       // ClientMessage
  ev[1] = 32;       // format = 32
  wire::wr16_le(ev + 2, ctx.transport().lastSeq());
  wire::wr32_le(ev + 4, xid);                        // window
  wire::wr32_le(ev + 8, atom::kWM_PROTOCOLS);        // type = WM_PROTOCOLS
  wire::wr32_le(ev + 12, atom::kWM_DELETE_WINDOW);   // data[0] = WM_DELETE_WINDOW
  wire::wr32_le(ev + 16, x11_now_ms_monotonic());    // data[1] = timestamp
  // data[2..4] = 0 (already zeroed)
  (void)ctx.transport().sendEvent32(xid, ev);

#ifndef NDEBUG
  TS_FPRINTF("[WINDOW_CLOSE] sent WM_DELETE_WINDOW to xid=0x%08X\n",
          (unsigned)xid);
#endif
}

void XProtoDaemon::drainHostCommands() {
  if (!server_) return;

  auto cmds = server_->hostCmds().takeAll();
  if (cmds.empty()) return;

  // Collect fds that need forceful disconnect (WindowClose without WM_DELETE_WINDOW).
  std::vector<int> forceDisconnect;

  for (const auto& c : cmds) {
    // Find the owning client for this command's window
    WindowView wv{};
    int owner_fd = -1;
    if (c.xid && server_->ctx().windows().snapshot(c.xid, wv)) {
      owner_fd = wv.owner_fd;
    }

    ClientSession* cs = (owner_fd > 0) ? findClient(owner_fd) : nullptr;

    // ---- ScreenLayoutChanged: notify ALL clients of new screen dimensions ----
    // Sends both ConfigureNotify on root (for raw X11 / Xt) AND
    // RRScreenChangeNotify (so Xlib's XRRUpdateConfiguration() updates
    // the cached WidthOfScreen / HeightOfScreen used for popup clipping).
    if (c.type == HostCmdType::ScreenLayoutChanged) {
      const auto layout = x11::getScreenLayout();
      const uint16_t rw = layout.virtual_w;
      const uint16_t rh = layout.virtual_h;
      const uint16_t rw_mm = layout.virtual_w_mm;
      const uint16_t rh_mm = layout.virtual_h_mm;
      TS_FPRINTF("[SCREEN_NOTIFY] sending ConfigureNotify + RRScreenChangeNotify "
              "to %zu client(s), new size=%dx%d mm=%dx%d\n",
              clients_.size(), (int)rw, (int)rh, (int)rw_mm, (int)rh_mm);

      const uint32_t now = x11_now_ms_monotonic();

      for (auto& [fd, session] : clients_) {
        activateClient(session);

        // 1) ConfigureNotify on root window (event type 22)
        {
          std::array<uint8_t, 32> ev{};
          ev[0] = 22; // ConfigureNotify
          ev[1] = 0;
          wire::wr16_le(ev.data() + 2, session.seq);
          wire::wr32_le(ev.data() + 4, kRootXid);  // event
          wire::wr32_le(ev.data() + 8, kRootXid);  // window
          wire::wr32_le(ev.data() + 12, 0);         // above-sibling: None
          wire::wr16_le(ev.data() + 16, 0);         // x
          wire::wr16_le(ev.data() + 18, 0);         // y
          wire::wr16_le(ev.data() + 20, rw);        // width
          wire::wr16_le(ev.data() + 22, rh);        // height
          wire::wr16_le(ev.data() + 24, 0);         // border_width
          ev[26] = 0; // override_redirect = false
          server_->ctx().transport().sendAll(ev.data(), 32);
        }

        // 2) RRScreenChangeNotify (event type = RANDR first_event + 0 = 89)
        //    This causes Xlib's XRRUpdateConfiguration() to update cached
        //    screen dimensions (WidthOfScreen/HeightOfScreen).
        {
          std::array<uint8_t, 32> ev{};
          ev[0] = ext::kRANDR_FirstEvent; // 89 = RRScreenChangeNotify
          ev[1] = 0;  // rotation (0 = RR_Rotate_0)
          wire::wr16_le(ev.data() + 2, session.seq);
          wire::wr32_le(ev.data() + 4, now);        // timestamp
          wire::wr32_le(ev.data() + 8, now);        // configTimestamp
          wire::wr32_le(ev.data() + 12, kRootXid);  // root
          wire::wr32_le(ev.data() + 16, kRootXid);  // requestWindow (root)
          wire::wr16_le(ev.data() + 20, 0);         // sizeID
          wire::wr16_le(ev.data() + 22, 0);         // subpixelOrder
          wire::wr16_le(ev.data() + 24, rw);        // widthInPixels
          wire::wr16_le(ev.data() + 26, rh);        // heightInPixels
          wire::wr16_le(ev.data() + 28, rw_mm);     // widthInMillimeters
          wire::wr16_le(ev.data() + 30, rh_mm);     // heightInMillimeters
          server_->ctx().transport().sendAll(ev.data(), 32);
        }

        deactivateClient();
      }
      continue;
    }

    // ---- WindowMoved: user dragged NSWindow — send ConfigureNotify ----
    // Java/Swing (Vivado) caches the window's root position and only updates
    // it when it receives ConfigureNotify. Without this, menu item tracking
    // uses stale coordinates after a cross-monitor drag.
    if (c.type == HostCmdType::WindowMoved) {
      if (!cs) continue;
      activateClient(*cs);

      x11::WindowView vw{};
      if (server_->ctx().windows().snapshot(c.xid, vw)) {
        std::array<uint8_t, 32> ev{};
        ev[0] = 22; // ConfigureNotify
        ev[1] = 0;
        wire::wr16_le(ev.data() + 2, cs->seq);
        wire::wr32_le(ev.data() + 4, c.xid);  // event
        wire::wr32_le(ev.data() + 8, c.xid);  // window
        wire::wr32_le(ev.data() + 12, 0);      // above-sibling: None
        wire::wr16_le(ev.data() + 16, static_cast<uint16_t>(vw.x));
        wire::wr16_le(ev.data() + 18, static_cast<uint16_t>(vw.y));
        wire::wr16_le(ev.data() + 20, vw.w);
        wire::wr16_le(ev.data() + 22, vw.h);
        wire::wr16_le(ev.data() + 24, vw.border_width);
        ev[26] = vw.override_redirect ? 1 : 0;
        server_->ctx().transport().sendAll(ev.data(), 32);
      }

      deactivateClient();
      continue;
    }

    // ---- WindowClose: special handling (needs daemon-level removeClient) ----
    if (c.type == HostCmdType::WindowClose) {
      if (!cs) continue;
      activateClient(*cs);

      if (windowSupportsDeleteProtocol(c.xid)) {
        // ICCCM-compliant: send ClientMessage, client will exit gracefully
        sendDeleteWindowMessage(server_->ctx(), c.xid);
        server_->flushNotifyQueue();
      } else {
        // Client doesn't support WM_DELETE_WINDOW — send UnmapNotify +
        // DestroyNotify so the client tears down this window gracefully.
        // NEVER disconnect the whole client just because one popup lacks
        // the WM_DELETE_WINDOW protocol (e.g. JidePopup in Vivado).
        {
          char buf[128];
          snprintf(buf, sizeof(buf),
            "[WINDOW_CLOSE] no WM_DELETE_WINDOW on xid=0x%08X fd=%d — "
            "sending Unmap+Destroy (not disconnecting)\n",
            (unsigned)c.xid, owner_fd);
          x11_ui_push_log(1, buf);
        }

        // UnmapNotify (type 18)
        {
          std::array<uint8_t, 32> ev{};
          ev[0] = 18; // UnmapNotify
          wire::wr32_le(ev.data() + 4, c.xid);  // event window
          wire::wr32_le(ev.data() + 8, c.xid);  // window
          ev[12] = 0; // from_configure = false
          server_->ctx().transport().sendAll(ev.data(), 32);
        }

        // DestroyNotify (type 17)
        {
          std::array<uint8_t, 32> ev{};
          ev[0] = 17; // DestroyNotify
          wire::wr32_le(ev.data() + 4, c.xid);  // event window
          wire::wr32_le(ev.data() + 8, c.xid);  // window
          server_->ctx().transport().sendAll(ev.data(), 32);
        }

      }

      deactivateClient();
      continue;
    }

    // For non-input host commands that update server-wide state (e.g.,
    // ScreenLayoutChanged, SetPresentable), we need a client active.
    // But for input events (Key, Button, PointerMove, ScrollTicks, Focus),
    // only deliver to the owning client — never a fallback. Delivering
    // input to the wrong client corrupts its XCB sequence state (the
    // event arrives mid-reply, causing disconnect).
    if (!cs && !clients_.empty()) {
      // Skip input events if owner not found (window was destroyed)
      if (c.type == HostCmdType::Key ||
          c.type == HostCmdType::Button ||
          c.type == HostCmdType::PointerMove ||
          c.type == HostCmdType::ScrollTicks ||
          c.type == HostCmdType::Focus) {
        continue;
      }
      cs = &clients_.begin()->second;
    }

    if (!cs) continue; // no clients at all

    // For PointerMove events (high-frequency, non-critical), check if the
    // client socket can accept data.  If the socket buffer is full (client
    // not reading — e.g., Java EDT in menu tracking), skip the event to
    // prevent the xproto thread from blocking in sendAll's EAGAIN loop.
    // Critical events (Button, Focus, WindowClose, etc.) are never skipped.
    //
    // EXCEPTION: During an active pointer grab (GrabPointer — used by menu
    // popups), the client explicitly requested PointerMotion events via the
    // grab event_mask.  Dropping them causes multi-second menu highlighting
    // delays.  Bypass the throttle so motion reaches the client even under
    // backpressure from heavy PutImage traffic.
    if (c.type == HostCmdType::PointerMove) {
      x11::PointerGrab pg{};
      bool haveGrab = server_->ctx().grabs().getPointerGrab(pg) && pg.active;
      if (!haveGrab) {
        struct pollfd pfd = { cs->client->fd(), POLLOUT, 0 };
        if (::poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLOUT)) {
          static int skip_count = 0;
          if (++skip_count <= 3 || (skip_count % 100) == 0) {
            TS_FPRINTF("[THROTTLE] skipped PointerMove #%d (socket not writable fd=%d)\n",
                    skip_count, cs->client->fd());
          }
          continue; // socket not writable — skip this motion event
        }
      }
    }

    activateClient(*cs);

    // Process this single command directly (no re-queue roundtrip).
    x11_proto_bridge_process_host_cmd(&c);
    server_->flushNotifyQueue();

    deactivateClient();
  }

  // Forcefully disconnect clients that don't support WM_DELETE_WINDOW.
  for (int fd : forceDisconnect) {
    removeClient(fd);
  }
}


} // namespace x11
