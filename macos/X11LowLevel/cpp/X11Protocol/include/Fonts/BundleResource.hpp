//
//  BundleResource.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#pragma once
#include <string>

std::string loadFrameworkTextResource(const char* nameNoExt,
                                      const char* ext,
                                      const char* subdir);
