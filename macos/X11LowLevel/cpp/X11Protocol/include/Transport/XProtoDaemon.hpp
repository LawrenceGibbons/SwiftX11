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
#include <string>

namespace x11 {

class XProtoServer;
class XProtoModules;
class XClient;

/// Return value for readAndDispatch — indicates what happened.
enum class DispatchResult {
  Dispatched,   // A complete request was dispatched
  NeedMore,     // Partial read or EAGAIN — no more data available now
  Eof,          // Clean EOF (recv returned 0) — remove client, not an error
  Error         // Connection error or protocol error — remove client, log as error
};

/// Ring buffer entry for the last N dispatched requests (crash diagnosis).
struct DispatchRecord {
  uint8_t  major = 0;
  uint8_t  minor = 0;
  uint16_t seq   = 0;
  bool     reply_sent = false;
};

/// Per-connection session state managed by the daemon's poll loop.
struct ClientSession {
  XClient* client = nullptr;
  uint16_t seq = 0;

  // Incremental request read state.
  // Phase 0: reading 4-byte header (hdr[0..3]).
  // Phase 0.5: reading 4-byte extended length (ext_len[0..3]) when BIG-REQUESTS
  //            is enabled and hdr len_words==0.
  // Phase 1: reading payload (buf[0..buf_need-1]).
  uint8_t hdr[4]{};
  size_t hdr_have = 0;          // bytes of header received (0–4)

  uint8_t ext_len[4]{};         // BIG-REQUESTS extended length (4 bytes)
  size_t ext_len_have = 0;      // bytes of extended length received (0–4)
  bool reading_ext_len = false;  // true when in phase 0.5

  std::vector<uint8_t> buf;
  size_t buf_have = 0;          // payload bytes received so far
  size_t buf_need = 0;          // total payload bytes needed

  // Ring buffer of last 16 dispatched requests for crash diagnosis.
  static constexpr int kHistorySize = 16;
  DispatchRecord history[16]{};
  int history_idx = 0;          // next write position (wraps)

  // XI2 diagnostic counters — track which XI2 requests this client sent.
  uint32_t total_requests = 0;
  bool sent_xi_query_version = false;   // minor 47
  bool sent_xi_query_device  = false;   // minor 48
  bool sent_xi_select_events = false;   // minor 46
  bool sent_list_input_devices = false; // minor 2

  // Set to true when the client closes cleanly (recv returned 0).
  // Controls log verbosity in removeClient() — clean disconnects skip the
  // ring-buffer dumps since they're only useful for protocol error diagnosis.
  bool clean_disconnect = false;

  void recordDispatch(uint8_t maj, uint8_t min, uint16_t s, bool replied) {
    auto& r = history[history_idx % kHistorySize];
    r.major = maj; r.minor = min; r.seq = s; r.reply_sent = replied;
    history_idx++;
    total_requests++;
  }
};

class XProtoDaemon {
public:
  XProtoDaemon();
  ~XProtoDaemon();

  bool start(int display, bool enableTCP = true, bool enableUnix = true,
             const char* tcpBindAddr = "0.0.0.0");
  void stop();

  bool isRunning() const { return running_.load(std::memory_order_acquire); }

  // Access the persistent server (created lazily on first client session).
  XProtoServer* server() { return server_; }

  // Find a client session by fd (used by host command routing).
  ClientSession* findClient(int fd);

  // Send a 32-byte event to a window, routing to the owning client's
  // transport regardless of which client is currently active.
  // Used for cross-client event delivery (focus, selection, crossing).
  bool sendEventCrossClient(uint32_t targetWid, const uint8_t ev[32]);

  // Variable-length version (for XI2 GenericEvent).
  bool sendEventCrossClientVariable(uint32_t targetWid, const uint8_t* ev, size_t len);

private:
  void runListener(int display, bool enableTCP, bool enableUnix,
                   const char* tcpBindAddr);
  void ensureServer();

  // Poll-loop helpers
  void acceptClient(int listenFd);
  void removeClient(int fd);
  DispatchResult readAndDispatch(int fd, ClientSession& cs);
  void drainHostCommands();
  void activateClient(ClientSession& cs);
  void deactivateClient();
  void flushPendingMaps();

  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::vector<int> listen_fds_;
  std::string unix_socket_path_;
  std::thread th_;

  // Active client sessions keyed by socket fd.
  std::unordered_map<int, ClientSession> clients_;

  // Persistent server-wide state (survives across client sessions).
  XProtoServer*  server_  = nullptr;
  XProtoModules* modules_ = nullptr;

  bool notifyBridgeInited_ = false;
};

} // namespace x11
