//
//  XProtoDaemon.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/3/26.
//

#include "Transport/XProtoDaemon.hpp"

#include <sys/socket.h>
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
#include "Ops/EventOps.hpp"
#include "Transport/X11Setup.hpp"

extern "C" void x11_cpp_notify_init(void* ctx_ptr, void* event_ops_ptr, void* queue_ptr);
extern "C" void x11_cpp_notify_shutdown(void);
extern "C" void x11_ui_push_destroy(uint32_t xid);

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

bool XProtoDaemon::start(int display, const char* bindAddr) {
  if (running_.load(std::memory_order_acquire)) return true;

  stop_.store(false, std::memory_order_release);
  th_ = std::thread([this, display, bindAddr] {
    runListener(display, bindAddr);
  });

  return true;
}

void XProtoDaemon::stop() {
  stop_.store(true, std::memory_order_release);

  // Closing listen fd unblocks poll
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }

  if (th_.joinable()) th_.join();
}

ClientSession* XProtoDaemon::findClient(int fd) {
  auto it = clients_.find(fd);
  return (it != clients_.end()) ? &it->second : nullptr;
}


// ---------- poll loop ----------

void XProtoDaemon::runListener(int display, const char* bindAddr) {
  running_.store(true, std::memory_order_release);

  listen_fd_ = make_listen_socket(display, bindAddr);
  if (listen_fd_ < 0) {
    running_.store(false, std::memory_order_release);
    return;
  }

  ensureServer();

  while (!stop_.load(std::memory_order_acquire)) {
    // Build pollfd array: listen socket + all client fds
    std::vector<struct pollfd> fds;
    fds.push_back({listen_fd_, POLLIN, 0});

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

    // Check listen socket for new connections
    if (fds[0].revents & POLLIN) {
      acceptClient();
    }

    // Drain host commands FIRST so client requests (e.g. xeyes' QueryPointer)
    // see the freshest InputState (pointer position updated by PointerMove).
    drainHostCommands();

    // Check client sockets
    std::vector<int> toRemove;
    for (size_t i = 0; i < clientFds.size(); i++) {
      int cfd = clientFds[i];
      short rev = fds[i + 1].revents; // +1 because listen is at index 0

      if (rev & (POLLHUP | POLLERR)) {
        toRemove.push_back(cfd);
        continue;
      }

      if (rev & POLLIN) {
        auto* cs = findClient(cfd);
        if (cs && !readAndDispatch(cfd, *cs)) {
          toRemove.push_back(cfd);
        }
      }
    }

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


void XProtoDaemon::acceptClient() {
  int cfd = ::accept(listen_fd_, nullptr, nullptr);
  if (cfd < 0) return;

#if defined(SO_NOSIGPIPE)
  int one = 1;
  ::setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

  // ---- Setup handshake (blocking — tiny, happens once per client) ----
  uint8_t req[12];
  if (!recv_waitall(cfd, req, sizeof(req))) {
    ::close(cfd);
    return;
  }

  const char byte_order = (char)req[0];
  if (byte_order != 'l') {
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
    ::close(cfd);
    return;
  }

  // ---- Allocate client ID space ----
  uint32_t clientSlot = g_nextClientSlot.fetch_add(1, std::memory_order_relaxed);
  if (clientSlot == 0) clientSlot = g_nextClientSlot.fetch_add(1, std::memory_order_relaxed);

  const uint32_t rid_mask = 0x00FFFFFFu;
  const uint32_t rid_base = (clientSlot & 0xFFu) << 24;

  x11_send_setup_success_minimal_little_endian(cfd, rid_base, rid_mask);

#ifndef NDEBUG
  fprintf(stderr, "[X11] accept fd=%d slot=%u rid_base=0x%08X rid_mask=0x%08X clients=%zu\n",
          cfd, (unsigned)clientSlot, (unsigned)rid_base, (unsigned)rid_mask,
          clients_.size() + 1);
#endif

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

#ifndef NDEBUG
  fprintf(stderr, "[X11] disconnect fd=%d remaining=%zu\n",
          fd, clients_.size() - 1);
#endif

  // Activate this client for teardown
  activateClient(cs);

  // Erase windows owned by this client
  std::vector<uint32_t> owned = server_->ctx().windows().eraseOwnedBy(fd);
  for (uint32_t wid : owned) {
    x11_ui_push_destroy(wid);
  }

  // Remove grabs for destroyed windows
  server_->ctx().grabs().removeForWindows(owned);

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


bool XProtoDaemon::readAndDispatch(int fd, ClientSession& cs) {
  // Phase 0: read 4-byte request header
  if (cs.hdr_have < 4 && !cs.reading_ext_len) {
    ssize_t r = ::recv(fd, cs.hdr + cs.hdr_have, 4 - cs.hdr_have, 0);
    if (r == 0) return false; // EOF
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
      return false;
    }
    cs.hdr_have += (size_t)r;
    if (cs.hdr_have < 4) return true; // need more header bytes

    // Header complete — parse it
    const uint16_t len_words = (uint16_t)(cs.hdr[2] | ((uint16_t)cs.hdr[3] << 8));

    if (len_words == 0) {
      // BIG-REQUESTS: len_words==0 means next 4 bytes are the 32-bit length
      if (cs.client && cs.client->bigReqEnabled()) {
        cs.reading_ext_len = true;
        cs.ext_len_have = 0;
        // fall through to phase 0.5
      } else {
        return false; // protocol error — BIG-REQUESTS not enabled
      }
    } else {
      const size_t total = (size_t)len_words * 4u;
      if (total < 4u) return false;

      cs.buf_need = total - 4u;
      cs.buf_have = 0;

      if (cs.buf_need > 0) {
        cs.buf.resize(cs.buf_need);
        return true; // wait for payload on next poll
      }

      // No payload — fall through to dispatch
    }
  }

  // Phase 0.5: read 4-byte extended length (BIG-REQUESTS)
  if (cs.reading_ext_len && cs.ext_len_have < 4) {
    ssize_t r = ::recv(fd, cs.ext_len + cs.ext_len_have, 4 - cs.ext_len_have, 0);
    if (r == 0) return false;
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
      return false;
    }
    cs.ext_len_have += (size_t)r;
    if (cs.ext_len_have < 4) return true; // need more bytes

    // Parse 32-bit extended length in words (little-endian)
    const uint32_t ext_words = (uint32_t)cs.ext_len[0]
                             | ((uint32_t)cs.ext_len[1] << 8)
                             | ((uint32_t)cs.ext_len[2] << 16)
                             | ((uint32_t)cs.ext_len[3] << 24);

    if (ext_words < 2) return false; // minimum is 2 words (8 bytes: 4 hdr + 4 ext_len)

    // Total bytes = ext_words * 4, already consumed 4 (hdr) + 4 (ext_len) = 8
    const size_t total = (size_t)ext_words * 4u;
    cs.buf_need = total - 8u;
    cs.buf_have = 0;
    cs.reading_ext_len = false;

    if (cs.buf_need > 0) {
      cs.buf.resize(cs.buf_need);
      return true; // wait for payload on next poll
    }
    // No additional payload — fall through to dispatch
  }

  // Phase 1: read payload
  if (cs.buf_need > 0 && cs.buf_have < cs.buf_need) {
    ssize_t r = ::recv(fd, cs.buf.data() + cs.buf_have,
                       cs.buf_need - cs.buf_have, 0);
    if (r == 0) return false;
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
      return false;
    }
    cs.buf_have += (size_t)r;
    if (cs.buf_have < cs.buf_need) return true; // need more payload bytes
  }

  // Full request ready — dispatch
  const uint8_t major = cs.hdr[0];
  const uint8_t minor = cs.hdr[1];
  const uint8_t* payload = cs.buf_need > 0 ? cs.buf.data() : nullptr;
  const size_t remain = cs.buf_need;

  cs.seq = (uint16_t)(cs.seq + 1);

  activateClient(cs);
  server_->ctx().transport().noteLastSeq(cs.seq);
  (void)server_->dispatch(major, minor, cs.seq, payload, remain);

  // Flush pending notifies for this client only (NOT host commands —
  // those are drained separately in drainHostCommands with correct
  // per-client activation).
  server_->flushNotifyQueue();

  deactivateClient();

  // Reset for next request
  cs.hdr_have = 0;
  cs.buf_need = 0;
  cs.buf_have = 0;
  cs.reading_ext_len = false;
  cs.ext_len_have = 0;

  return true;
}


void XProtoDaemon::drainHostCommands() {
  if (!server_) return;

  auto cmds = server_->hostCmds().takeAll();
  if (cmds.empty()) return;

  for (const auto& c : cmds) {
    // Find the owning client for this command's window
    WindowView wv{};
    int owner_fd = -1;
    if (c.xid && server_->ctx().windows().snapshot(c.xid, wv)) {
      owner_fd = wv.owner_fd;
    }

    ClientSession* cs = (owner_fd > 0) ? findClient(owner_fd) : nullptr;

    // For input events that update server-wide InputState, we need a client
    // active for event delivery. If no specific owner found, use any client.
    if (!cs && !clients_.empty()) {
      cs = &clients_.begin()->second;
    }

    if (!cs) continue; // no clients at all

    activateClient(*cs);

    // Process this single command directly (no re-queue roundtrip).
    x11_proto_bridge_process_host_cmd(&c);
    server_->flushNotifyQueue();

    deactivateClient();
  }
}


} // namespace x11
