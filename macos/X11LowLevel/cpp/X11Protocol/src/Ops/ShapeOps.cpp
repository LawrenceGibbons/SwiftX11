//
//  ShapeOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "ShapeOps.hpp"

// Temporary stand-ins so this compiles before you introduce shared headers.
// Delete these once XProtoServerState / XProtoRequestContext exist for real.
struct XProtoServerState {};
struct XProtoRequestContext {};

void ShapeOps::handlePolyFillRectangle(int /*clientFd*/, uint16_t /*seq*/,
                                       const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO: PolyFillRectangle (major=70)
  // - parse: drawable, gc, list of xRectangle {x,y,width,height}
  // - resolve drawable -> window fb or pixmap pixels (later: depth/format aware)
  // - apply GC fg color (for now)
  // - fill rects; clip to bounds
  // - if drawable is a window: enqueue damage
}

void ShapeOps::handlePolyFillArc(int /*clientFd*/, uint16_t /*seq*/,
                                 const uint8_t* /*payload*/, std::size_t /*len*/) {
  // TODO: PolyFillArc (major=71)
  // - parse: drawable, gc, list of xArc {x,y,width,height,angle1,angle2}
  // - resolve drawable -> buffer
  // - if extent ~= full circle: fill ellipse
  // - else: fill arc wedge using X11 angle convention (degrees*64, CCW from +X)
  // - enqueue damage if drawable is a window
}
