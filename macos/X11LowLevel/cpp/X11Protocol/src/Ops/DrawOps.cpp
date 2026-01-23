//
//  DrawOps.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 1/19/26.
//

#include "DrawOps.hpp"

struct XProtoServerState {};
struct XProtoRequestContext {};

void DrawOps::handlePutImage(uint8_t /*format*/, const uint8_t* /*payload*/, std::size_t /*len*/) {
  // Stub: implement PutImage here later.
  // This will eventually:
  //  - resolve drawable (window/pixmap)
  //  - handle XYPixmap depth=1 (write into packed bits if dst depth==1)
  //  - handle ZPixmap depth=24/32 (copy bytes into dst pixels)
}

void DrawOps::handleCopyArea(const uint8_t* /*payload*/, std::size_t /*len*/) {
  // Stub: implement CopyArea here later.
  // This will eventually:
  //  - resolve src drawable (window/pixmap)
  //  - resolve dst drawable (window/pixmap; but NOT depth=1 via pixels pointer)
  //  - do a clipped blit
}

void DrawOps::handleCopyPlane(const uint8_t* /*payload*/, std::size_t /*len*/) {
  // Stub: implement CopyPlane here later.
  // This will eventually:
  //  - read from packed depth=1 source pixmap bits
  //  - write either packed depth=1 bits OR mapped fg/bg pixels into dst
}
