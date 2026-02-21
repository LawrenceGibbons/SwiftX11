//
//  XProtoModules.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#pragma once
#include <Ops/QueryOps.hpp>
#include <Ops/AtomOps.hpp>
#include <Ops/WindowOps.hpp>
#include <Ops/WindowAttrOps.hpp>
#include <Ops/PropOps.hpp>
#include <Ops/GCOps.hpp>
#include <Ops/DrawOps.hpp>
#include <Ops/PixmapOps.hpp>
#include <Ops/ShapeOps.hpp>
#include <Ops/FontOps.hpp>
#include <Ops/ColorOps.hpp>
#include <Ops/CursorOps.hpp>
#include <Ops/GrabOps.hpp>
#include <Ops/PointerOps.hpp>

namespace x11 {
  
  struct XProtoModules {
    QueryOps      queryOps;
    AtomOps       atomOps;
    WindowOps     windowOps;
    WindowAttrOps windowAttrOps;
    PropOps       propOps;
    GCOps         gcOps;
    DrawOps       drawOps;
    PixmapOps     pixmapOps;
    ShapeOps      shapeOps;
    FontOps       fontOps;
    ColorOps      colorOps;
    CursorOps     cursorOps;
    GrabOps       grabOps;
    PointerOps    pointerOps;
    // etc
    
    explicit XProtoModules(XProtoRegistrar& reg)
    : queryOps(reg)
    , atomOps(reg)
    , windowOps(reg)
    , windowAttrOps(reg)
    , propOps(reg)
    , gcOps(reg)
    , drawOps(reg)
    , pixmapOps(reg)
    , shapeOps(reg)
    , fontOps(reg)
    , colorOps(reg)
    , cursorOps(reg)
    , grabOps(reg)
    , pointerOps(reg)
    {}
  };
}
