//
//  XProtoDaemon.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/3/26.
//

#pragma once

#include <atomic>
#include <thread>

namespace x11 {

class XProtoDaemon {
public:
  XProtoDaemon();
  ~XProtoDaemon();

  bool start(int display, const char* bindAddr = "127.0.0.1");
  void stop();

  bool isRunning() const { return running_.load(std::memory_order_acquire); }

private:
  void runListener(int display, const char* bindAddr);
  void runSession(int client_fd);

  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::thread th_;
};

} // namespace x11
