//
//  ScreenLayout.hpp
//  X11LowLevel
//
//  Cached per-monitor display configuration queried from CoreGraphics.
//  Provides dynamic screen geometry for RANDR, Xinerama, GetGeometry,
//  and the X11 connection setup handshake.
//
//  Thread-safe: getScreenLayout() can be called from any thread.
//  The cache is refreshed on CGDisplay reconfiguration events.
//

#pragma once

#include <cstdint>
#include <vector>

namespace x11 {

/// Information about a single physical monitor.
struct MonitorInfo {
    uint32_t cg_display_id = 0;   // CGDirectDisplayID

    // RANDR resource XIDs (stable as long as monitor count doesn't change)
    uint32_t output_xid = 0;
    uint32_t crtc_xid   = 0;
    uint32_t mode_xid   = 0;

    // Position in X11 root coordinate space (points, top-left origin)
    int16_t  x = 0, y = 0;

    // Size in points (X11 units)
    uint16_t w = 0, h = 0;

    // Pixel dimensions (for RANDR mode info)
    uint16_t w_px = 0, h_px = 0;

    // Physical size in millimeters (from CGDisplayScreenSize)
    uint16_t w_mm = 0, h_mm = 0;

    // Human-readable name ("Virtual-0", "Virtual-1", etc.)
    char name[32] = {};

    bool is_primary = false;
};

/// Snapshot of the full display layout.
struct ScreenLayout {
    // Virtual desktop = union bounding box of all monitors
    uint16_t virtual_w     = 800;
    uint16_t virtual_h     = 600;
    uint16_t virtual_w_mm  = 270;
    uint16_t virtual_h_mm  = 203;

    // Per-monitor information
    std::vector<MonitorInfo> monitors;
};

/// Thread-safe accessor for the cached screen layout.
/// Returns a copy so the caller is safe from concurrent refresh.
ScreenLayout getScreenLayout();

/// Force-refresh the cached layout from CoreGraphics.
/// Called automatically by the display-change callback.
void refreshScreenLayout();

/// Register a CGDisplayReconfigurationCallback so the cache is
/// automatically refreshed when monitors are added/removed/rearranged.
/// Call once at server startup.
void registerDisplayChangeCallback();

} // namespace x11
