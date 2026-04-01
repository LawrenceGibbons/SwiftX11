//
//  IncrTransfer.hpp
//  X11Protocol
//
//  Manages active INCR (incremental) clipboard transfers.
//  When the server acts as selection owner (macOS clipboard → X11 client),
//  data larger than kChunkSize is delivered in chunks via the INCR protocol.
//
//  Single-threaded: all access from the xproto thread. No mutex needed.
//

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include "Core/PropertyTable.hpp"
#include "Core/ClipboardAtoms.hpp"

namespace x11 {

class IncrTransfer {
public:
  static constexpr size_t kChunkSize = 64 * 1024; // 64 KB per chunk

  static IncrTransfer& instance() {
    static IncrTransfer t;
    return t;
  }

  struct Transfer {
    uint32_t requestor;
    uint32_t property;
    uint32_t target;       // UTF8_STRING, STRING, etc.
    int      owner_fd;     // client fd (for disconnect cleanup)
    std::vector<uint8_t> data;
    size_t   offset = 0;
  };

  /// Start a new INCR transfer. Writes the INCR property (type=INCR,
  /// value=total size) and returns the total data size.
  uint32_t startTransfer(uint32_t requestor, uint32_t property,
                         uint32_t target, int owner_fd,
                         std::vector<uint8_t>&& data) {
    uint32_t totalSize = (uint32_t)data.size();
    uint64_t k = key(requestor, property);
    Transfer t;
    t.requestor = requestor;
    t.property  = property;
    t.target    = target;
    t.owner_fd  = owner_fd;
    t.data      = std::move(data);
    t.offset    = 0;
    transfers_[k] = std::move(t);

    // Write INCR property: type=INCR, format=32, value=total size estimate
    uint8_t sizeLE[4];
    sizeLE[0] = (uint8_t)(totalSize      );
    sizeLE[1] = (uint8_t)(totalSize >>  8);
    sizeLE[2] = (uint8_t)(totalSize >> 16);
    sizeLE[3] = (uint8_t)(totalSize >> 24);
    PropertyTable::instance().setReplace(
      requestor, property,
      atom::kINCR, 32,
      sizeLE, 4
    );
    return totalSize;
  }

  /// Check if an active transfer exists for (requestor, property).
  bool hasTransfer(uint32_t requestor, uint32_t property) const {
    return transfers_.count(key(requestor, property)) > 0;
  }

  /// Advance the transfer: write the next chunk (or zero-length terminator).
  /// Returns true if a transfer was found and advanced.
  /// Writes to PropertyTable; caller must send PropertyNotify(NewValue).
  bool writeNextChunk(uint32_t requestor, uint32_t property) {
    uint64_t k = key(requestor, property);
    auto it = transfers_.find(k);
    if (it == transfers_.end()) return false;

    Transfer& t = it->second;
    size_t remaining = t.data.size() - t.offset;
    size_t chunk = std::min(remaining, kChunkSize);

    if (chunk > 0) {
      // Write next chunk
      PropertyTable::instance().setReplace(
        requestor, property,
        t.target, 8,
        t.data.data() + t.offset, chunk
      );
      t.offset += chunk;
    } else {
      // Write zero-length property to signal completion
      PropertyTable::instance().setReplace(
        requestor, property,
        t.target, 8,
        nullptr, 0
      );
      transfers_.erase(it);
    }
    return true;
  }

  /// Cancel all transfers for a given client fd (disconnect cleanup).
  void cancelForFd(int fd) {
    for (auto it = transfers_.begin(); it != transfers_.end(); ) {
      if (it->second.owner_fd == fd)
        it = transfers_.erase(it);
      else
        ++it;
    }
  }

private:
  IncrTransfer() = default;

  static uint64_t key(uint32_t wid, uint32_t atom) {
    return (uint64_t(wid) << 32) | uint64_t(atom);
  }

  std::unordered_map<uint64_t, Transfer> transfers_;
};

} // namespace x11
