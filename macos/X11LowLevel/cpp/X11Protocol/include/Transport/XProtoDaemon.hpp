//
//  XProtoDaemon.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/3/26.
//

#pragma once

#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace x11 {

class XProtoServer;
class XProtoModules;
class XClient;

/// Per-connection session state managed by the daemon's poll loop.
struct ClientSession {
  XClient* client = nullptr;
  uint16_t seq = 0;

  // Incremental request read state.
  // Phase 0: reading 4-byte header (hdr[0..3]).
  // Phase 1: reading payload (buf[0..buf_need-1]).
  uint8_t hdr[4]{};
  size_t hdr_have = 0;          // bytes of header received (0–4)

  std::vector<uint8_t> buf;
  size_t buf_have = 0;          // payload bytes received so far
  size_t buf_need = 0;          // total payload bytes needed
};

class XProtoDaemon {
public:
  XProtoDaemon();
  ~XProtoDaemon();

  bool start(int display, const char* bindAddr = "127.0.0.1");
  void stop();

  bool isRunning() const { return running_.load(std::memory_order_acquire); }

  // Access the persistent server (created lazily on first client session).
  XProtoServer* server() { return server_; }

  // Find a client session by fd (used by host command routing).
  ClientSession* findClient(int fd);

private:
  void runListener(int display, const char* bindAddr);
  void ensureServer();

  // Poll-loop helpers
  void acceptClient();
  void removeClient(int fd);
  bool readAndDispatch(int fd, ClientSession& cs);
  void drainHostCommands();
  void activateClient(ClientSession& cs);
  void deactivateClient();

  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::thread th_;

  // Active client sessions keyed by socket fd.
  std::unordered_map<int, ClientSession> clients_;

  // Persistent server-wide state (survives across client sessions).
  XProtoServer*  server_  = nullptr;
  XProtoModules* modules_ = nullptr;

  bool notifyBridgeInited_ = false;
};

} // namespace x11
