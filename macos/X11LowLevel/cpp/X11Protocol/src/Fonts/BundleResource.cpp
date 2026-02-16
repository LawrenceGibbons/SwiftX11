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
  if (!url) {
    // Helpful diagnostics when resource lookup fails.
    CFStringRef bundleID = CFBundleGetIdentifier(bundle);
    char bundleIDBuf[256] = {0};
    if (bundleID) {
      CFStringGetCString(bundleID, bundleIDBuf, (CFIndex)sizeof(bundleIDBuf), kCFStringEncodingUTF8);
    } else {
      std::snprintf(bundleIDBuf, sizeof(bundleIDBuf), "<no-bundle-id>");
    }

    CFURLRef resURL = CFBundleCopyResourcesDirectoryURL(bundle);
    char resPathBuf[1024] = {0};
    if (resURL) {
      CFURLGetFileSystemRepresentation(resURL, true, (UInt8*)resPathBuf, (CFIndex)sizeof(resPathBuf));
      CFRelease(resURL);
    } else {
      std::snprintf(resPathBuf, sizeof(resPathBuf), "<no-resources-dir>");
    }

    const char* which =
        (bundle == CFBundleGetMainBundle()) ? "main-bundle" : "named-bundle";

    std::fprintf(stderr,
                 "[BundleResource] MISS (%s) bundleId=%s resourcesDir=%s "
                 "name=\"%s\" ext=\"%s\" subdir=\"%s\"\n",
                 which,
                 bundleIDBuf,
                 resPathBuf,
                 nameNoExt ? nameNoExt : "<null>",
                 ext ? ext : "<null>",
                 (subdir && subdir[0]) ? subdir : "<none>");
    
    std::fprintf(stderr,
                 "[BundleResource] expectedRelPath=%s%s%s.%s\n",
                 (subdir && subdir[0]) ? subdir : "",
                 (subdir && subdir[0]) ? "/" : "",
                 nameNoExt ? nameNoExt : "<null>",
                 ext ? ext : "<null>");

    return {};
  }

  if (cfSubdir) CFRelease(cfSubdir);
  CFRelease(cfName);
  CFRelease(cfExt);

  if (!url) return {};
  std::string s = readFileURLToString(url);
  CFRelease(url);
  return s;
}
