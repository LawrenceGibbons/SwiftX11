//
//  X11CursorShape.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/20/26.
//

// Core/X11CursorShape.hpp
#pragma once
#include <cstdint>

namespace x11 {

// Keep this stable across the C bridge.
enum class X11CursorShape : int32_t {
  Arrow               = 0,
  IBeam               = 1,
  Crosshair           = 2,
  PointingHand        = 3,
  OpenHand            = 4,
  ClosedHand          = 5,
  ResizeLeftRight     = 6,
  ResizeUpDown        = 7,
  ResizeDiagonal      = 8,
  OperationNotAllowed = 9,
  ContextualMenu      = 10,
};

// Map X11 cursorfont glyph (XC_*) -> coarse NSCursor shape.
// Values from X11/cursorfont.h.  [oai_citation:1‡CoCalc](https://cocalc.com/github/emscripten-core/emscripten/blob/main/system/include/X11/cursorfont.h)
static inline X11CursorShape shapeFromCursorfontGlyph(uint16_t glyph) {
  switch (glyph) {
    // text / terminal cursor
    case 152: /* XC_xterm */ return X11CursorShape::IBeam;

    // pointers
    case 68:  /* XC_left_ptr */
    case 94:  /* XC_right_ptr */
    case 2:   /* XC_arrow */
      return X11CursorShape::Arrow;

    // crosshair-ish
    case 34:  /* XC_crosshair */
    case 30:  /* XC_cross */
      return X11CursorShape::Crosshair;

    // hands
    case 58:  /* XC_hand1 */
    case 60:  /* XC_hand2 */
      return X11CursorShape::PointingHand;

    // move / sizing
    case 52:  /* XC_fleur */
    case 120: /* XC_sizing */
      return X11CursorShape::OpenHand;

    // resize L/R
    case 70:  /* XC_left_side */
    case 96:  /* XC_right_side */
    case 108: /* XC_sb_h_double_arrow */
      return X11CursorShape::ResizeLeftRight;

    // resize U/D
    case 16:  /* XC_bottom_side */
    case 138: /* XC_top_side */
    case 116: /* XC_sb_v_double_arrow */
      return X11CursorShape::ResizeUpDown;

    // resize diagonals (corners + angles)
    case 12:  /* XC_bottom_left_corner */
    case 14:  /* XC_bottom_right_corner */
    case 134: /* XC_top_left_corner */
    case 136: /* XC_top_right_corner */
    case 76:  /* XC_ll_angle */
    case 78:  /* XC_lr_angle */
    case 144: /* XC_ul_angle */
    case 148: /* XC_ur_angle */
      return X11CursorShape::ResizeDiagonal;

    // “help”ish
    case 92:  /* XC_question_arrow */
      return X11CursorShape::ContextualMenu;

    default:
      return X11CursorShape::Arrow;
  }
}

} // namespace x11
