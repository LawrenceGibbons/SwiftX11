#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace x11 {

  struct ClipRect {
    int16_t  x = 0, y = 0;
    uint16_t w = 0, h = 0;
  };

  struct GCState {
    uint32_t xid = 0;

    // Core fields xterm/text needs
    uint32_t fg  = 0xFF000000u; // black
    uint32_t bg  = 0xFFFFFFFFu; // white
    uint32_t font = 0;          // fid (CARD32), 0 means "server default"

    // Keep for correctness (you can ignore in renderer for now but store them)
    uint8_t  function = 3;      // GXcopy (3) default per X11
    uint32_t plane_mask = 0xFFFFFFFFu;

    // Clip state (SetClipRectangles / CreateGC / ChangeGC)
    int16_t  clip_x_origin = 0;
    int16_t  clip_y_origin = 0;
    std::vector<ClipRect> clip_rects;
    bool     has_clip = false;  // false=None (no clipping), true=active clip
    // Note: has_clip=true with empty clip_rects means "clip everything"

    // Line/fill/arc attributes (bits 4-13, 20-22)
    uint16_t line_width    = 0;     // bit 4
    uint8_t  line_style    = 0;     // bit 5  (0=Solid, 1=OnOffDash, 2=DoubleDash)
    uint8_t  cap_style     = 1;     // bit 6  (0=NotLast, 1=Butt, 2=Round, 3=Projecting)
    uint8_t  join_style    = 0;     // bit 7  (0=Miter, 1=Round, 2=Bevel)
    uint8_t  fill_style    = 0;     // bit 8  (0=Solid, 1=Tiled, 2=Stippled, 3=OpaqueStippled)
    uint8_t  fill_rule     = 0;     // bit 9  (0=EvenOdd, 1=Winding)
    uint32_t tile          = 0;     // bit 10 (pixmap XID)
    uint32_t stipple       = 0;     // bit 11 (pixmap XID)
    int16_t  ts_x_origin   = 0;    // bit 12
    int16_t  ts_y_origin   = 0;    // bit 13

    bool     graphics_exposures = true; // bit 16
    uint8_t  subwindow_mode = 0;        // bit 15 (0=ClipByChildren, 1=IncludeInferiors)

    uint16_t dash_offset   = 0;    // bit 20
    uint8_t  dashes_single = 4;    // bit 21 (single dash length)
    uint8_t  arc_mode      = 0;    // bit 22 (0=Chord, 1=PieSlice)

    // SetDashes (opcode 58) dash list
    std::vector<uint8_t> dash_list = {4, 4};
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
