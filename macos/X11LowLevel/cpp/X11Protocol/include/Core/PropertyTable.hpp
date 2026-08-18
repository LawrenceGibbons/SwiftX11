//
//  PropertyTable.hpp
//  X11LowLevel
//
//  In-process property storage (thread-safe singleton).
//  Used by PropOps (ChangeProperty/GetProperty) and SelectionOps (clipboard bridge).
//

#pragma once
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace x11 {

class PropertyTable {
public:
  static PropertyTable& instance() {
    static PropertyTable t;
    return t;
  }

  struct Prop {
    uint32_t type = 0;    // Atom
    uint8_t  format = 0;  // 0/8/16/32
    std::vector<uint8_t> data;
  };

  bool get(uint32_t wid, uint32_t atom, Prop& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = map_.find(key(wid, atom));
    if (it == map_.end()) return false;
    out = it->second;
    return true;
  }

  void setReplace(uint32_t wid, uint32_t atom, uint32_t type, uint8_t format,
                  const uint8_t* bytes, std::size_t n) {
    std::lock_guard<std::mutex> lock(mu_);
    auto& p = map_[key(wid, atom)];
    p.type = type;
    p.format = format;
    p.data.assign(bytes ? bytes : nullptr, bytes ? bytes + n : nullptr);
    cap_(p.data);
  }

  void setAppend(uint32_t wid, uint32_t atom, uint32_t type, uint8_t format,
                 const uint8_t* bytes, std::size_t n, bool append) {
    std::lock_guard<std::mutex> lock(mu_);
    auto k = key(wid, atom);

    auto it = map_.find(k);
    if (it == map_.end() || it->second.type != type || it->second.format != format) {
      auto& p = map_[k];
      p.type = type;
      p.format = format;
      p.data.assign(bytes ? bytes : nullptr, bytes ? bytes + n : nullptr);
      cap_(p.data);
      return;
    }

    auto& p = it->second;
    if (!bytes || n == 0) return;

    if (append) {
      p.data.insert(p.data.end(), bytes, bytes + n);
    } else {
      p.data.insert(p.data.begin(), bytes, bytes + n);
    }
    cap_(p.data);
  }

  void erase(uint32_t wid, uint32_t atom) {
    std::lock_guard<std::mutex> lock(mu_);
    map_.erase(key(wid, atom));
  }

  bool listAtoms(uint32_t wid, std::vector<uint32_t>& outAtoms) const {
    std::lock_guard<std::mutex> lock(mu_);
    outAtoms.clear();
    outAtoms.reserve(16);

    for (const auto& kv : map_) {
      const uint64_t k = kv.first;
      const uint32_t w = (uint32_t)(k >> 32);
      if (w == wid) {
        const uint32_t atom = (uint32_t)(k & 0xFFFFFFFFu);
        outAtoms.push_back(atom);
      }
    }

    std::sort(outAtoms.begin(), outAtoms.end());
    outAtoms.erase(std::unique(outAtoms.begin(), outAtoms.end()), outAtoms.end());
    return true;
  }

private:
  PropertyTable() = default;
  PropertyTable(const PropertyTable&) = delete;
  PropertyTable& operator=(const PropertyTable&) = delete;

  static uint64_t key(uint32_t wid, uint32_t atom) {
    return (uint64_t(wid) << 32) | uint64_t(atom);
  }

  static void cap_(std::vector<uint8_t>& v) {
    constexpr std::size_t kMax = (3u << 24); // 48 MB safety valve
    if (v.size() > kMax) v.resize(kMax);
  }

  mutable std::mutex mu_;
  std::unordered_map<uint64_t, Prop> map_;
};

} // namespace x11
