//
//  ShapeRegion.hpp
//  X11LowLevel
//
//  X11 SHAPE extension region storage.
//  Stores a union of rectangles defining a non-rectangular window shape.
//

#pragma once
#include <cstdint>
#include <vector>

namespace x11 {

struct ShapeRegion {
    struct Rect {
        int16_t  x, y;
        uint16_t w, h;
    };

    std::vector<Rect> rects;   // union of rectangles defining the region
    bool shaped = false;       // false = full window rect (unshaped)

    // --- Mutators --------------------------------------------------------

    // Set region from explicit rectangle list.
    // op: Set=0, Union=1, Intersect=2, Subtract=3, Invert=4
    void setFromRects(const Rect* rects, size_t count,
                      int16_t x_off, int16_t y_off,
                      uint16_t win_w, uint16_t win_h,
                      uint8_t op);

    // Set region from a depth-1 bitmap (run-length encodes set bits).
    // stride_bytes is the bitmap's row stride in bytes.
    // LSBFirst bit order (matching server's bitmap_bit_order).
    void setFromBitmap(const uint8_t* bits, uint16_t bw, uint16_t bh,
                       int stride_bytes,
                       int16_t x_off, int16_t y_off,
                       uint16_t win_w, uint16_t win_h,
                       uint8_t op);

    // Combine with another region using the given op.
    void combine(const ShapeRegion& other, uint8_t op,
                 int16_t x_off, int16_t y_off,
                 uint16_t win_w, uint16_t win_h);

    // Translate all rectangles by (dx, dy).
    void offset(int16_t dx, int16_t dy);

    // Reset to unshaped (full window rect).
    void reset();

    // --- Queries ---------------------------------------------------------

    // Test if point (px, py) in window-local coords is inside the region.
    bool contains(int16_t px, int16_t py) const;

    // Compute bounding extents of all rectangles.
    Rect extents() const;

private:
    // Apply shape operation: merges `incoming` rects into this region.
    void applyOp(const std::vector<Rect>& incoming, uint8_t op,
                 uint16_t win_w, uint16_t win_h);
};

} // namespace x11
