//
//  XProtoModules.hpp
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/24/26.
//

#pragma once
#include "QueryOps.hpp"
#include "AtomOps.hpp"

namespace x11 {
  
  struct XProtoModules {
    QueryOps queryOps;
    AtomOps atomOps;
    // etc
    
    explicit XProtoModules(XProtoRegistrar& reg)
    : queryOps(reg)
    , atomOps(reg)
    {}
  };
}
