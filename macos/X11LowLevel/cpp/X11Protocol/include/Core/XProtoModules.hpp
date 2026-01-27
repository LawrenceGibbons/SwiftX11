//
//  XProtoModules.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#pragma once
#include "QueryOps.hpp"
#include "AtomOps.hpp"
#include "WindowOps.hpp"
#include "WindowAttrOps.hpp"
#include "PropOps.hpp"

namespace x11 {
  
  struct XProtoModules {
    QueryOps  queryOps;
    AtomOps   atomOps;
    WindowOps windowOps;
    WindowAttrOps windowAttrOps;
    PropOps propOps;
    // etc
    
    explicit XProtoModules(XProtoRegistrar& reg)
    : queryOps(reg)
    , atomOps(reg)
    , windowOps(reg)
    , windowAttrOps(reg)
    , propOps(reg)
    {}
  };
}
