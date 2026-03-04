//
//  ShapeRegion.cpp
//  X11LowLevel
//
//  X11 SHAPE extension region storage implementation.
//

#include "Core/ShapeRegion.hpp"
#include <algorithm>
#include <cstring>

namespace x11 {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Test if two rects overlap.
static bool rectsOverlap(const ShapeRegion::Rect& a, const ShapeRegion::Rect& b) {
    return a.x < (int16_t)(b.x + b.w) && (int16_t)(a.x + a.w) > b.x &&
           a.y < (int16_t)(b.y + b.h) && (int16_t)(a.y + a.h) > b.y;
}

// Intersect two rects. Returns false if no overlap.
static bool rectIntersect(const ShapeRegion::Rect& a, const ShapeRegion::Rect& b,
                          ShapeRegion::Rect& out) {
    int16_t x1 = std::max(a.x, b.x);
    int16_t y1 = std::max(a.y, b.y);
    int16_t x2 = std::min((int16_t)(a.x + a.w), (int16_t)(b.x + b.w));
    int16_t y2 = std::min((int16_t)(a.y + a.h), (int16_t)(b.y + b.h));
    if (x1 >= x2 || y1 >= y2) return false;
    out = { x1, y1, (uint16_t)(x2 - x1), (uint16_t)(y2 - y1) };
    return true;
}

// Full-window rectangle.
static ShapeRegion::Rect fullRect(uint16_t w, uint16_t h) {
    return { 0, 0, w, h };
}

// ---------------------------------------------------------------------------
// applyOp — merge incoming rects into this region using the SHAPE operation
// ---------------------------------------------------------------------------
void ShapeRegion::applyOp(const std::vector<Rect>& incoming, uint8_t op,
                          uint16_t win_w, uint16_t win_h) {
    switch (op) {
    case 0: // ShapeSet — replace
        rects = incoming;
        shaped = !rects.empty();
        break;

    case 1: // ShapeUnion — add incoming rects
        rects.insert(rects.end(), incoming.begin(), incoming.end());
        shaped = !rects.empty();
        break;

    case 2: { // ShapeIntersect — keep only overlapping areas
        std::vector<Rect> result;
        // If not currently shaped, treat as full window rect
        std::vector<Rect> current = shaped ? rects : std::vector<Rect>{ fullRect(win_w, win_h) };
        for (auto& cr : current) {
            for (auto& ir : incoming) {
                Rect isect{};
                if (rectIntersect(cr, ir, isect)) {
                    result.push_back(isect);
                }
            }
        }
        rects = std::move(result);
        shaped = true;
        break;
    }

    case 3: { // ShapeSubtract — remove incoming from current
        // If not currently shaped, start with full window
        if (!shaped) {
            rects = { fullRect(win_w, win_h) };
            shaped = true;
        }
        // For each incoming rect, subtract it from all current rects
        for (auto& sub : incoming) {
            std::vector<Rect> result;
            for (auto& cr : rects) {
                if (!rectsOverlap(cr, sub)) {
                    result.push_back(cr);
                    continue;
                }
                // Split cr into up to 4 pieces around sub
                int16_t cx2 = cr.x + cr.w;
                int16_t cy2 = cr.y + cr.h;
                int16_t sx1 = std::max(cr.x, sub.x);
                int16_t sy1 = std::max(cr.y, sub.y);
                int16_t sx2 = std::min(cx2, (int16_t)(sub.x + sub.w));
                int16_t sy2 = std::min(cy2, (int16_t)(sub.y + sub.h));

                // Top strip
                if (cr.y < sy1)
                    result.push_back({ cr.x, cr.y, cr.w, (uint16_t)(sy1 - cr.y) });
                // Bottom strip
                if (cy2 > sy2)
                    result.push_back({ cr.x, sy2, cr.w, (uint16_t)(cy2 - sy2) });
                // Left strip (between top and bottom)
                if (cr.x < sx1)
                    result.push_back({ cr.x, sy1, (uint16_t)(sx1 - cr.x), (uint16_t)(sy2 - sy1) });
                // Right strip (between top and bottom)
                if (cx2 > sx2)
                    result.push_back({ sx2, sy1, (uint16_t)(cx2 - sx2), (uint16_t)(sy2 - sy1) });
            }
            rects = std::move(result);
        }
        break;
    }

    case 4: { // ShapeInvert — XOR-like: (window \ current) ∪ (incoming \ window)
        // For simplicity, treat as Set (most clients don't use Invert)
        rects = incoming;
        shaped = !rects.empty();
        break;
    }

    default:
        // Unknown op — treat as Set
        rects = incoming;
        shaped = !rects.empty();
        break;
    }
}

// ---------------------------------------------------------------------------
// setFromRects
// ---------------------------------------------------------------------------
void ShapeRegion::setFromRects(const Rect* r, size_t count,
                               int16_t x_off, int16_t y_off,
                               uint16_t win_w, uint16_t win_h,
                               uint8_t op) {
    std::vector<Rect> incoming;
    incoming.reserve(count);
    for (size_t i = 0; i < count; i++) {
        incoming.push_back({ (int16_t)(r[i].x + x_off),
                             (int16_t)(r[i].y + y_off),
                             r[i].w, r[i].h });
    }
    applyOp(incoming, op, win_w, win_h);
}

// ---------------------------------------------------------------------------
// setFromBitmap — run-length encode set bits into rectangles
// ---------------------------------------------------------------------------
void ShapeRegion::setFromBitmap(const uint8_t* bits, uint16_t bw, uint16_t bh,
                                int stride_bytes,
                                int16_t x_off, int16_t y_off,
                                uint16_t win_w, uint16_t win_h,
                                uint8_t op) {
    std::vector<Rect> incoming;

    for (uint16_t row = 0; row < bh; row++) {
        const uint8_t* rowPtr = bits + row * stride_bytes;
        int runStart = -1;

        for (uint16_t col = 0; col < bw; col++) {
            // LSBFirst bit order: byte[col/8], bit (col%8) from LSB
            bool set = (rowPtr[col >> 3] >> (col & 7)) & 1;
            if (set && runStart < 0) {
                runStart = col;
            } else if (!set && runStart >= 0) {
                incoming.push_back({ (int16_t)(runStart + x_off),
                                     (int16_t)(row + y_off),
                                     (uint16_t)(col - runStart), 1 });
                runStart = -1;
            }
        }
        // Close any open run at end of row
        if (runStart >= 0) {
            incoming.push_back({ (int16_t)(runStart + x_off),
                                 (int16_t)(row + y_off),
                                 (uint16_t)(bw - runStart), 1 });
        }
    }

    applyOp(incoming, op, win_w, win_h);
}

// ---------------------------------------------------------------------------
// combine
// ---------------------------------------------------------------------------
void ShapeRegion::combine(const ShapeRegion& other, uint8_t op,
                          int16_t x_off, int16_t y_off,
                          uint16_t win_w, uint16_t win_h) {
    // Offset the other region's rects
    std::vector<Rect> incoming;
    incoming.reserve(other.rects.size());
    for (auto& r : other.rects) {
        incoming.push_back({ (int16_t)(r.x + x_off),
                             (int16_t)(r.y + y_off),
                             r.w, r.h });
    }
    applyOp(incoming, op, win_w, win_h);
}

// ---------------------------------------------------------------------------
// offset
// ---------------------------------------------------------------------------
void ShapeRegion::offset(int16_t dx, int16_t dy) {
    for (auto& r : rects) {
        r.x += dx;
        r.y += dy;
    }
}

// ---------------------------------------------------------------------------
// reset
// ---------------------------------------------------------------------------
void ShapeRegion::reset() {
    rects.clear();
    shaped = false;
}

// ---------------------------------------------------------------------------
// contains
// ---------------------------------------------------------------------------
bool ShapeRegion::contains(int16_t px, int16_t py) const {
    if (!shaped) return true; // unshaped = full window
    for (auto& r : rects) {
        if (px >= r.x && px < r.x + r.w &&
            py >= r.y && py < r.y + r.h) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// extents
// ---------------------------------------------------------------------------
ShapeRegion::Rect ShapeRegion::extents() const {
    if (rects.empty()) return { 0, 0, 0, 0 };
    int16_t x1 = rects[0].x, y1 = rects[0].y;
    int16_t x2 = rects[0].x + rects[0].w;
    int16_t y2 = rects[0].y + rects[0].h;
    for (size_t i = 1; i < rects.size(); i++) {
        x1 = std::min(x1, rects[i].x);
        y1 = std::min(y1, rects[i].y);
        x2 = std::max(x2, (int16_t)(rects[i].x + rects[i].w));
        y2 = std::max(y2, (int16_t)(rects[i].y + rects[i].h));
    }
    return { x1, y1, (uint16_t)(x2 - x1), (uint16_t)(y2 - y1) };
}

} // namespace x11
