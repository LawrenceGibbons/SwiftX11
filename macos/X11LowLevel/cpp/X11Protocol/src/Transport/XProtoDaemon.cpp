//
//  XProtoDaemon.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/3/26.
//

#include "XProtoDaemon.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include "XProtoServerBridge.h"   // reuse begin/end if you want initially
// If you don’t want to use begin/end bridge functions anymore, we’ll inline that logic next.
#include "XProtoServer.hpp"
#include "x11_setup.h"

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
      if (r == 0) return false; // EOF
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
  
  // returns:  1 ok, 0 disconnect, -2 timeout, -1 fatal
  static int recv_hdr_timeout(int fd, uint8_t hdr[4]) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    timeval tv{0, 100 * 1000};
    int sel = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (sel == 0) return -2;
    if (sel < 0) {
      if (errno == EINTR) return -2;
      return -1;
    }
    return recv_waitall(fd, hdr, 4) ? 1 : 0;
  }
  
  // ---------- lifecycle ----------
  XProtoDaemon::XProtoDaemon() = default;
  XProtoDaemon::~XProtoDaemon() { stop(); }
  
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
    
    // Closing listen fd unblocks accept
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    
    if (th_.joinable()) th_.join();
  }
  
  void XProtoDaemon::runListener(int display, const char* bindAddr) {
    running_.store(true, std::memory_order_release);
    
    listen_fd_ = make_listen_socket(display, bindAddr);
    if (listen_fd_ < 0) {
      running_.store(false, std::memory_order_release);
      return;
    }
    
    while (!stop_.load(std::memory_order_acquire)) {
      int cfd = ::accept(listen_fd_, nullptr, nullptr);
      if (cfd < 0) {
        if (errno == EINTR) continue;
        if (stop_.load(std::memory_order_acquire)) break;
        continue;
      }
      
      
#if defined(SO_NOSIGPIPE)
      int one = 1;
      ::setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
      
      // One client at a time (same as your current design)
      runSession(cfd);
      
      ::shutdown(cfd, SHUT_RDWR);
      ::close(cfd);
    }
    
    running_.store(false, std::memory_order_release);
  }
  
//  static bool recv_exact(int fd, void* buf, size_t n) {
//    uint8_t* p = (uint8_t*)buf;
//    size_t got = 0;
//    while (got < n) {
//      ssize_t r = ::recv(fd, p + got, n - got, 0);
//      if (r == 0) return false;
//      if (r < 0) {
//        if (errno == EINTR) continue;
//        return false;
//      }
//      got += (size_t)r;
//    }
//    return true;
//  }
  
  // ---------- the meat: setup + drain ----------
  void XProtoDaemon::runSession(int cfd) {
    // Match your C behavior: small recv timeout so we can notice stop.
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100 * 1000;
    ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv));
    
    // ----- Setup request: read 12-byte client prefix -----
    uint8_t req[12];
    if (!recv_waitall(cfd, req, sizeof(req))) return;
    
    const char byte_order = (char)req[0];
    if (byte_order != 'l') {
      x11_send_setup_failed_le(cfd, "SwiftX11: only little-endian supported");
      return;
    }
    
    // auth lengths in bytes [6..9]
    const uint16_t auth_proto_len = (uint16_t)(req[6] | ((uint16_t)req[7] << 8));
    const uint16_t auth_data_len  = (uint16_t)(req[8] | ((uint16_t)req[9] << 8));
    
    size_t skip = 0;
    skip += ((size_t)auth_proto_len + 3u) & ~3u;
    skip += ((size_t)auth_data_len  + 3u) & ~3u;
    
    if (skip && !skip_bytes(cfd, skip)) return;
    
    // Reply minimal SetupSuccess
    x11_send_setup_success_minimal_little_endian(cfd);
    
    // ----- Begin C++ session for this client -----
    x11_proto_bridge_begin_session(cfd);
    
    // ----- Drain requests -----
    uint16_t seq = 0;
    
    for (;;) {
      x11_proto_bridge_flush_notify_queue();
      if (stop_.load(std::memory_order_acquire)) break;
      
      uint8_t hdr[4];
      int hr = recv_hdr_timeout(cfd, hdr);
      if (hr == -2) continue; // timeout -> loop again (flush runs at top)
      if (hr <= 0) break;     // disconnect or error
      
      const uint8_t major = hdr[0];
      const uint8_t minor = hdr[1];
      const uint16_t len_words = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));
      if (len_words == 0) break;
      
      const size_t total = (size_t)len_words * 4u;
      if (total < 4u) break;
      const size_t remain = total - 4u;
      
      seq = (uint16_t)(seq + 1);
      x11_proto_bridge_note_last_seq(seq);
      
      uint8_t stack_buf[4096];
      uint8_t* payload = stack_buf;
      std::unique_ptr<uint8_t[]> heap;
      
      if (remain > sizeof(stack_buf)) {
        heap.reset(new (std::nothrow) uint8_t[remain]);
        if (!heap) break;
        payload = heap.get();
      }
      
      if (remain && !recv_waitall(cfd, payload, remain)) break;
      
      (void)x11_proto_bridge_dispatch(major, minor, seq, payload, remain);
      
      x11_proto_bridge_flush_notify_queue();
    }
    
    x11_proto_bridge_end_session(cfd);
  }
  
} // namespace x11
