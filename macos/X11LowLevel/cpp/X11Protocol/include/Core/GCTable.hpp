#pragma once
#include <cstdint>
#include <unordered_map>
#include <mutex>

namespace x11 {
  
  struct GCState {
    uint32_t xid = 0;
    
    
    // Core fields xterm/text needs
    uint32_t fg  = 0xFF000000u; // black
//    uint32_t fg  = 0xFF00FF00u; // green
    uint32_t bg  = 0xFFFFFFFFu; // white
    uint32_t font = 0;          // fid (CARD32), 0 means "server default"
    
    // Keep for correctness (you can ignore in renderer for now but store them)
    uint8_t  function = 3;      // GXcopy (3) default per X11
    uint32_t plane_mask = 0xFFFFFFFFu;
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
