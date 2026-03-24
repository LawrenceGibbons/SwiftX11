//
//  CursorRouting.cpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/20/26.
//

#include <cstdio>

#include "Core/CursorRouting.hpp"
#include "UI/UICommandQueue.hpp"
#include "Core/X11CursorShape.hpp"   // the enum + mapping above
#include "Core/CursorTable.hpp" 
#include "Utils/MachTime.hpp"

  // Call when you have a host (top-level) and a target (deepest under pointer).
  void x11::maybeApplyCursor(x11::XProtoContext& ctx, uint32_t host, uint32_t target) {
    if (!host) return;
    
    const uint32_t cursorXid = x11::resolveEffectiveCursorCid(ctx, target ? target : host);
    
    if (ctx.input().last_cursor_host == host && ctx.input().last_cursor_cid == cursorXid) {
      return; // unchanged
    }
    
    ctx.input().last_cursor_host = host;
    ctx.input().last_cursor_cid  = cursorXid;
    
    x11::X11CursorShape shape = x11::X11CursorShape::Arrow;
    
    if (cursorXid != 0) {
      x11::CursorTable::Cursor c{};
      if (ctx.cursors().get(cursorXid, c)) {
        if (c.kind == x11::CursorTable::Cursor::Kind::Glyph) {
          shape = x11::shapeFromCursorfontGlyph(c.source_char);
        } else {
          // Pixmap cursor: for now default to Arrow (until you render / inspect pixmaps)
          shape = x11::X11CursorShape::Arrow;
        }
      }
    }
    
#ifdef X11_TRACE_VERBOSE
    TS_FPRINTF("[CURSOR_APPLY] host=0x%08X target=0x%08X cursor=0x%08X shape=%d\n",
            (unsigned)host, (unsigned)target, (unsigned)cursorXid, (int)shape);
#endif
    
    ctx.ui().push(x11::UICommand{
      x11::UICommand::Type::SetCursor,
      /*xid=*/host,
      /*parent=*/0,
      /*x_px=*/0, /*y_px=*/0,
      /*w_px=*/0, /*h_px=*/0,
      /*flags=*/0,
      /*cursor_xid=*/cursorXid,
      /*shape=*/(int32_t)shape,
      /*title_utf8=*/nullptr
    });
  }
  

