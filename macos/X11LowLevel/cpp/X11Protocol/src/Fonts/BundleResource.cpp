//
//  BundleResource.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#include <CoreFoundation/CoreFoundation.h>
#include <string>
#include <vector>

static std::string readFileURLToString(CFURLRef url) {
  if (!url) return {};

  CFReadStreamRef stream = CFReadStreamCreateWithFile(kCFAllocatorDefault, url);
  if (!stream) return {};
  if (!CFReadStreamOpen(stream)) { CFRelease(stream); return {}; }

  std::string out;
  uint8_t buf[4096];
  while (true) {
    CFIndex n = CFReadStreamRead(stream, buf, (CFIndex)sizeof(buf));
    if (n <= 0) break;
    out.append(reinterpret_cast<const char*>(buf), (size_t)n);
  }

  CFReadStreamClose(stream);
  CFRelease(stream);
  return out;
}

// Load a resource file from the X11LowLevel.framework bundle.
// Example: loadFrameworkTextResource("fonts/fixed", "bdf")
std::string loadFrameworkTextResource(const char* nameNoExt, const char* ext, const char* subdir) {
  
  
  // Prefer the framework bundle (X11LowLevel.framework) rather than main bundle.
  // If you know your bundle identifier, you can use CFBundleGetBundleWithIdentifier.
  // Otherwise, CFBundleGetMainBundle may work when resources are copied into app.

  // Prefer the X11LowLevel.framework bundle (resources live there).
  CFBundleRef bundle = nullptr;
  CFStringRef bid = CFSTR("RLAN.X11LowLevel");
  bundle = CFBundleGetBundleWithIdentifier(bid);

  // Fallback: main bundle (only works if resources were copied there)
  if (!bundle) bundle = CFBundleGetMainBundle();
  if (!bundle) return {};

  CFStringRef cfName = CFStringCreateWithCString(kCFAllocatorDefault, nameNoExt, kCFStringEncodingUTF8);
  CFStringRef cfExt  = CFStringCreateWithCString(kCFAllocatorDefault, ext,       kCFStringEncodingUTF8);

  CFStringRef cfSubdir = nullptr;
  if (subdir && subdir[0]) {
    cfSubdir = CFStringCreateWithCString(kCFAllocatorDefault, subdir, kCFStringEncodingUTF8);
  }

  CFURLRef url = CFBundleCopyResourceURL(bundle, cfName, cfExt, cfSubdir);

  if (cfSubdir) CFRelease(cfSubdir);
  CFRelease(cfName);
  CFRelease(cfExt);

  if (!url) return {};
  std::string s = readFileURLToString(url);
  CFRelease(url);
  return s;
}
