//
//  DrawTrace.hpp
//  X11LowLevel
//
//  Diagnostic "Draw Trace" (Settings toggle): logs the clear/fill/text/blit
//  ops issued to WINDOW drawables so a stale-content bug (e.g. a tab label
//  that isn't cleared before a shifted redraw) can be diagnosed from the exact
//  op sequence.  Windows only — pixmap backbuffers issue far too many ops.
//
#pragma once

#include <cstdio>
#include <cstdint>
#include "Core/XProtoContext.hpp"
#include "Core/WindowTable.hpp"   // ctx.windows().exists()
#include "SwiftX11Bridge.h"

namespace x11 {

// A fill/clear/blit rectangle op.
inline void drawTraceRect(XProtoContext& ctx, const char* op, uint32_t drawable,
                          int x, int y, int w, int h) {
  if (!x11_get_draw_trace() || !ctx.windows().exists(drawable)) return;
  char buf[192];
  std::snprintf(buf, sizeof buf, "[DRAWSEQ] %-16s win=0x%08X rect=(%d,%d %dx%d)\n",
                op, (unsigned)drawable, x, y, w, h);
  x11_ui_push_log(1, buf);
}

// A text op (ImageText fills bg then draws; PolyText draws transparent).
inline void drawTraceText(XProtoContext& ctx, const char* op, uint32_t drawable,
                          int x, int y, const char* text, int len) {
  if (!x11_get_draw_trace() || !ctx.windows().exists(drawable)) return;
  char t[80];
  int n = (len < (int)sizeof(t) - 1) ? len : (int)sizeof(t) - 1;
  for (int i = 0; i < n; i++) { unsigned char c = (unsigned char)text[i]; t[i] = (c >= 32 && c < 127) ? (char)c : '.'; }
  t[n] = 0;
  char buf[256];
  std::snprintf(buf, sizeof buf, "[DRAWSEQ] %-16s win=0x%08X at (%d,%d) len=%d \"%s\"\n",
                op, (unsigned)drawable, x, y, len, t);
  x11_ui_push_log(1, buf);
}

} // namespace x11
