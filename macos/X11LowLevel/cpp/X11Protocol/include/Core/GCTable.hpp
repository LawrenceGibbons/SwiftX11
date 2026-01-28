#pragma once
#include <cstdint>
#include <unordered_map>
#include <mutex>

namespace x11 {

struct GCState {
  uint32_t xid = 0;
  uint32_t fg  = 0xFF000000u; // default black
  uint32_t bg  = 0xFFFFFFFFu; // default white
};

class GCTable {
public:
  static GCTable& instance();

  // Create if missing, return current state (by value).
  GCState getOrCreate(uint32_t gcXid);

  // Read (no create)
  bool find(uint32_t gcXid, GCState& out) const;

  // Write/overwrite
  void upsert(const GCState& st);

  void erase(uint32_t gcXid);

private:
  GCTable() = default;

  mutable std::mutex mu_;
  std::unordered_map<uint32_t, GCState> map_;
};

} // namespace x11
