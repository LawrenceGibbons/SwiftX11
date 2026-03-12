//
//  ScreenLayout.cpp
//  X11LowLevel
//
//  Queries CoreGraphics for the active display list and caches per-monitor
//  information for use by RANDR, Xinerama, GetGeometry, and the X11
//  connection setup handshake.
//

#include "Core/ScreenLayout.hpp"
#include "Core/HostCommandQueue.hpp"
#include "Core/XConstants.hpp"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>

#include <CoreGraphics/CoreGraphics.h>

// Forward: push a ScreenLayoutChanged host command so the server thread
// can send ConfigureNotify on the root window.
extern "C" void x11_proto_bridge_screen_layout_changed(void);

namespace x11 {

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

static std::mutex s_layout_mu;
static ScreenLayout s_layout;
static bool s_callback_registered = false;

static inline bool nearly_equal(double a, double b) {
    return std::fabs(a - b) < 1.0;
}

/// Build a fresh ScreenLayout from CoreGraphics.
static ScreenLayout buildLayout() {
    ScreenLayout layout;

    CGDirectDisplayID displays[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, displays, &count) != kCGErrorSuccess || count == 0) {
        // Return safe defaults (single 800x600 monitor)
        MonitorInfo m{};
        m.output_xid = 0x00100000;
        m.crtc_xid   = 0x00100001;
        m.mode_xid   = 0x00100010;
        m.w = 800; m.h = 600;
        m.w_px = 800; m.h_px = 600;
        m.w_mm = 270; m.h_mm = 203;
        m.is_primary = true;
        std::snprintf(m.name, sizeof(m.name), "Virtual-0");
        layout.monitors.push_back(m);
        return layout;
    }

    const CGDirectDisplayID mainDisp = CGMainDisplayID();

    // ---- Pass 1: Gather per-display info in CG coordinates ----
    // CGDisplayBounds uses a top-left-origin coordinate system where the
    // main display's top-left is (0,0).  This is the same orientation as
    // X11 root coordinates, so we can use them directly after shifting
    // the union origin to (0,0).

    struct RawDisplay {
        CGDirectDisplayID did;
        double x_u, y_u, w_u, h_u;  // position and size in points
        double w_px, h_px;            // pixel dimensions
        double w_mm, h_mm;            // physical mm
        bool is_primary;
    };
    std::vector<RawDisplay> raws;
    raws.reserve(count);

    for (uint32_t i = 0; i < count; i++) {
        const CGDirectDisplayID did = displays[i];
        CGRect b = CGDisplayBounds(did);

        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(did);
        double mode_w_u = 0, mode_h_u = 0, mode_w_px = 0, mode_h_px = 0;
        double scale_x = 1.0, scale_y = 1.0;

        if (mode) {
            mode_w_u  = static_cast<double>(CGDisplayModeGetWidth(mode));
            mode_h_u  = static_cast<double>(CGDisplayModeGetHeight(mode));
            mode_w_px = static_cast<double>(CGDisplayModeGetPixelWidth(mode));
            mode_h_px = static_cast<double>(CGDisplayModeGetPixelHeight(mode));
            if (mode_w_u > 0 && mode_w_px > 0) scale_x = mode_w_px / mode_w_u;
            if (mode_h_u > 0 && mode_h_px > 0) scale_y = mode_h_px / mode_h_u;
            if (scale_x <= 0.0) scale_x = 1.0;
            if (scale_y <= 0.0) scale_y = 1.0;
        }

        const double bw = CGRectGetWidth(b);
        const double bh = CGRectGetHeight(b);

        // Detect whether CGDisplayBounds returns pixels or units
        bool bounds_is_px = false;
        bool bounds_is_u  = false;
        if (mode) {
            if (mode_w_px > 0 && nearly_equal(bw, mode_w_px)) bounds_is_px = true;
            if (mode_w_u  > 0 && nearly_equal(bw, mode_w_u))  bounds_is_u  = true;
            if (!bounds_is_px && mode_h_px > 0 && nearly_equal(bh, mode_h_px)) bounds_is_px = true;
            if (!bounds_is_u  && mode_h_u  > 0 && nearly_equal(bh, mode_h_u))  bounds_is_u  = true;
        }

        double x_u = CGRectGetMinX(b);
        double y_u = CGRectGetMinY(b);
        double w_u = bw;
        double h_u = bh;

        if (bounds_is_px && !bounds_is_u) {
            x_u /= scale_x;
            y_u /= scale_y;
            w_u /= scale_x;
            h_u /= scale_y;
        }

        // Physical size in mm
        CGSize phys = CGDisplayScreenSize(did);  // mm
        double disp_w_mm = phys.width;
        double disp_h_mm = phys.height;
        if (disp_w_mm <= 0) disp_w_mm = w_u * 25.4 / 96.0;  // fallback: 96 DPI
        if (disp_h_mm <= 0) disp_h_mm = h_u * 25.4 / 96.0;

        RawDisplay rd;
        rd.did = did;
        rd.x_u = x_u;
        rd.y_u = y_u;
        rd.w_u = w_u;
        rd.h_u = h_u;
        rd.w_px = mode ? mode_w_px : w_u;
        rd.h_px = mode ? mode_h_px : h_u;
        rd.w_mm = disp_w_mm;
        rd.h_mm = disp_h_mm;
        rd.is_primary = (did == mainDisp);
        raws.push_back(rd);

        if (mode) CFRelease(mode);
    }

    // ---- Pass 2: Compute union bounding box ----
    double minX = raws[0].x_u, minY = raws[0].y_u;
    double maxX = raws[0].x_u + raws[0].w_u;
    double maxY = raws[0].y_u + raws[0].h_u;
    for (size_t i = 1; i < raws.size(); i++) {
        if (raws[i].x_u < minX) minX = raws[i].x_u;
        if (raws[i].y_u < minY) minY = raws[i].y_u;
        double rx = raws[i].x_u + raws[i].w_u;
        double ry = raws[i].y_u + raws[i].h_u;
        if (rx > maxX) maxX = rx;
        if (ry > maxY) maxY = ry;
    }

    double vw = maxX - minX;
    double vh = maxY - minY;
    if (vw < 1.0) vw = 1.0;
    if (vh < 1.0) vh = 1.0;
    if (vw > 65535.0) vw = 65535.0;
    if (vh > 65535.0) vh = 65535.0;

    layout.virtual_w = static_cast<uint16_t>(vw + 0.5);
    layout.virtual_h = static_cast<uint16_t>(vh + 0.5);

    // Virtual desktop physical mm: use main display's DPI to scale
    // (same approach as the original x11_get_virtual_desktop_px)
    {
        double main_mm_w = 0, main_mm_h = 0, main_u_w = 0, main_u_h = 0;
        for (auto& r : raws) {
            if (r.is_primary) {
                main_mm_w = r.w_mm;
                main_mm_h = r.h_mm;
                main_u_w  = r.w_u;
                main_u_h  = r.h_u;
                break;
            }
        }
        if (main_mm_w > 0 && main_u_w > 0) {
            double wmm = vw * (main_mm_w / main_u_w);
            double hmm = vh * (main_mm_h / main_u_h);
            if (wmm < 1.0) wmm = 1.0;
            if (hmm < 1.0) hmm = 1.0;
            if (wmm > 65535.0) wmm = 65535.0;
            if (hmm > 65535.0) hmm = 65535.0;
            layout.virtual_w_mm = static_cast<uint16_t>(wmm + 0.5);
            layout.virtual_h_mm = static_cast<uint16_t>(hmm + 0.5);
        }
    }

    // ---- Pass 3: Build MonitorInfo entries ----
    layout.monitors.reserve(raws.size());
    for (size_t i = 0; i < raws.size(); i++) {
        const auto& r = raws[i];
        MonitorInfo m{};
        m.cg_display_id = r.did;

        // Assign stable RANDR resource XIDs
        m.output_xid = 0x00100000 + static_cast<uint32_t>(i * 2);
        m.crtc_xid   = 0x00100001 + static_cast<uint32_t>(i * 2);
        m.mode_xid   = 0x00100010 + static_cast<uint32_t>(i);

        // Position relative to union origin (so X11 root (0,0) = union top-left)
        m.x = static_cast<int16_t>(r.x_u - minX + 0.5);
        m.y = static_cast<int16_t>(r.y_u - minY + 0.5);

        m.w = static_cast<uint16_t>(r.w_u + 0.5);
        m.h = static_cast<uint16_t>(r.h_u + 0.5);

        m.w_px = static_cast<uint16_t>(r.w_px + 0.5);
        m.h_px = static_cast<uint16_t>(r.h_px + 0.5);

        m.w_mm = static_cast<uint16_t>(r.w_mm + 0.5);
        m.h_mm = static_cast<uint16_t>(r.h_mm + 0.5);

        m.is_primary = r.is_primary;

        std::snprintf(m.name, sizeof(m.name), "Virtual-%zu", i);

        layout.monitors.push_back(m);
    }

    return layout;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ScreenLayout getScreenLayout() {
    std::lock_guard<std::mutex> lock(s_layout_mu);
    if (s_layout.monitors.empty()) {
        // First call — populate lazily
        s_layout = buildLayout();
        fprintf(stderr, "[SCREEN] initial layout: %dx%d, %zu monitor(s)\n",
                (int)s_layout.virtual_w, (int)s_layout.virtual_h,
                s_layout.monitors.size());
        for (auto& m : s_layout.monitors) {
            fprintf(stderr, "[SCREEN]   %s: pos=(%d,%d) size=%dx%d px=%dx%d mm=%dx%d%s\n",
                    m.name, (int)m.x, (int)m.y,
                    (int)m.w, (int)m.h,
                    (int)m.w_px, (int)m.h_px,
                    (int)m.w_mm, (int)m.h_mm,
                    m.is_primary ? " [primary]" : "");
        }
    }
    return s_layout;
}

void refreshScreenLayout() {
    ScreenLayout fresh = buildLayout();  // Build outside lock

    {
        std::lock_guard<std::mutex> lock(s_layout_mu);
        s_layout = std::move(fresh);
    }

    fprintf(stderr, "[SCREEN] layout refreshed: %dx%d, %zu monitor(s)\n",
            (int)s_layout.virtual_w, (int)s_layout.virtual_h,
            s_layout.monitors.size());
    for (auto& m : s_layout.monitors) {
        fprintf(stderr, "[SCREEN]   %s: pos=(%d,%d) size=%dx%d px=%dx%d mm=%dx%d%s\n",
                m.name, (int)m.x, (int)m.y,
                (int)m.w, (int)m.h,
                (int)m.w_px, (int)m.h_px,
                (int)m.w_mm, (int)m.h_mm,
                m.is_primary ? " [primary]" : "");
    }

    // Notify the X11 server thread to send ConfigureNotify on root window.
    x11_proto_bridge_screen_layout_changed();
}

// CGDisplayReconfigurationCallback signature
static void displayReconfigCallback(CGDirectDisplayID display,
                                     CGDisplayChangeSummaryFlags flags,
                                     void* /*userInfo*/) {
    // Only refresh on meaningful changes (not just mouse cursor moves etc.)
    if (flags & (kCGDisplayAddFlag | kCGDisplayRemoveFlag |
                 kCGDisplayMovedFlag | kCGDisplaySetModeFlag |
                 kCGDisplaySetMainFlag | kCGDisplayDesktopShapeChangedFlag)) {
        fprintf(stderr, "[SCREEN] display reconfiguration detected (flags=0x%X), refreshing\n",
                (unsigned)flags);
        refreshScreenLayout();
    }
}

void registerDisplayChangeCallback() {
    std::lock_guard<std::mutex> lock(s_layout_mu);
    if (!s_callback_registered) {
        CGDisplayRegisterReconfigurationCallback(displayReconfigCallback, nullptr);
        s_callback_registered = true;
    }
}

} // namespace x11
