//
//  x11_parameters.h
//  SwiftX11
//
//  Created by Lawrence Gibbons on 1/4/26.
//

// x11_parameters.h
#ifndef x11_parameters_h
#define x11_parameters_h

#ifdef __cplusplus
extern "C" {
#endif

// ---- Core backend limits ----

// Maximum number of simultaneous windows the backend supports.
// Root window (if any) is NOT counted here.
#define X11_MAX_WINDOWS 64

// ---- Future-proof placeholders ----

// Max depth of window tree (reparenting)
//#define X11_MAX_TREE_DEPTH 16

// Max number of grabs (pointer/keyboard)
//#define X11_MAX_GRABS 8

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* x11_parameters_h */
