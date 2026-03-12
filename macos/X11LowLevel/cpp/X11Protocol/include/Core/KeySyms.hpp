//
//  KeySyms.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/7/26.
//

#pragma once

static constexpr uint32_t XK_NoSymbol   = 0x00000000;

// TTY function keys
static constexpr uint32_t XK_BackSpace  = 0x0000FF08;
static constexpr uint32_t XK_Tab        = 0x0000FF09;
static constexpr uint32_t XK_Linefeed   = 0x0000FF0A;
static constexpr uint32_t XK_Clear      = 0x0000FF0B;
static constexpr uint32_t XK_Return     = 0x0000FF0D;
static constexpr uint32_t XK_Pause      = 0x0000FF13;
static constexpr uint32_t XK_Scroll_Lock = 0x0000FF14;
static constexpr uint32_t XK_Escape     = 0x0000FF1B;
static constexpr uint32_t XK_Delete     = 0x0000FFFF;

// Cursor motion & navigation
static constexpr uint32_t XK_Home       = 0x0000FF50;
static constexpr uint32_t XK_Left       = 0x0000FF51;
static constexpr uint32_t XK_Up         = 0x0000FF52;
static constexpr uint32_t XK_Right      = 0x0000FF53;
static constexpr uint32_t XK_Down       = 0x0000FF54;
static constexpr uint32_t XK_Page_Up    = 0x0000FF55;
static constexpr uint32_t XK_Page_Down  = 0x0000FF56;
static constexpr uint32_t XK_End        = 0x0000FF57;
static constexpr uint32_t XK_Begin      = 0x0000FF58;

// Misc function keys
static constexpr uint32_t XK_Print      = 0x0000FF61;
static constexpr uint32_t XK_Insert     = 0x0000FF63;
static constexpr uint32_t XK_Menu       = 0x0000FF67;
static constexpr uint32_t XK_Help       = 0x0000FF6A;
static constexpr uint32_t XK_Num_Lock   = 0x0000FF7F;

// Keypad
static constexpr uint32_t XK_KP_Space   = 0x0000FF80;
static constexpr uint32_t XK_KP_Tab     = 0x0000FF89;
static constexpr uint32_t XK_KP_Enter   = 0x0000FF8D;
static constexpr uint32_t XK_KP_F1      = 0x0000FF91;
static constexpr uint32_t XK_KP_F2      = 0x0000FF92;
static constexpr uint32_t XK_KP_F3      = 0x0000FF93;
static constexpr uint32_t XK_KP_F4      = 0x0000FF94;
static constexpr uint32_t XK_KP_Home    = 0x0000FF95;
static constexpr uint32_t XK_KP_Left    = 0x0000FF96;
static constexpr uint32_t XK_KP_Up      = 0x0000FF97;
static constexpr uint32_t XK_KP_Right   = 0x0000FF98;
static constexpr uint32_t XK_KP_Down    = 0x0000FF99;
static constexpr uint32_t XK_KP_Page_Up = 0x0000FF9A;
static constexpr uint32_t XK_KP_Page_Down = 0x0000FF9B;
static constexpr uint32_t XK_KP_End     = 0x0000FF9C;
static constexpr uint32_t XK_KP_Begin   = 0x0000FF9D;
static constexpr uint32_t XK_KP_Insert  = 0x0000FF9E;
static constexpr uint32_t XK_KP_Delete  = 0x0000FF9F;
static constexpr uint32_t XK_KP_Multiply = 0x0000FFAA;
static constexpr uint32_t XK_KP_Add     = 0x0000FFAB;
static constexpr uint32_t XK_KP_Separator = 0x0000FFAC;
static constexpr uint32_t XK_KP_Subtract = 0x0000FFAD;
static constexpr uint32_t XK_KP_Decimal = 0x0000FFAE;
static constexpr uint32_t XK_KP_Divide  = 0x0000FFAF;
static constexpr uint32_t XK_KP_0       = 0x0000FFB0;
static constexpr uint32_t XK_KP_1       = 0x0000FFB1;
static constexpr uint32_t XK_KP_2       = 0x0000FFB2;
static constexpr uint32_t XK_KP_3       = 0x0000FFB3;
static constexpr uint32_t XK_KP_4       = 0x0000FFB4;
static constexpr uint32_t XK_KP_5       = 0x0000FFB5;
static constexpr uint32_t XK_KP_6       = 0x0000FFB6;
static constexpr uint32_t XK_KP_7       = 0x0000FFB7;
static constexpr uint32_t XK_KP_8       = 0x0000FFB8;
static constexpr uint32_t XK_KP_9       = 0x0000FFB9;
static constexpr uint32_t XK_KP_Equal   = 0x0000FFBD;

// Function keys
static constexpr uint32_t XK_F1         = 0x0000FFBE;
static constexpr uint32_t XK_F2         = 0x0000FFBF;
static constexpr uint32_t XK_F3         = 0x0000FFC0;
static constexpr uint32_t XK_F4         = 0x0000FFC1;
static constexpr uint32_t XK_F5         = 0x0000FFC2;
static constexpr uint32_t XK_F6         = 0x0000FFC3;
static constexpr uint32_t XK_F7         = 0x0000FFC4;
static constexpr uint32_t XK_F8         = 0x0000FFC5;
static constexpr uint32_t XK_F9         = 0x0000FFC6;
static constexpr uint32_t XK_F10        = 0x0000FFC7;
static constexpr uint32_t XK_F11        = 0x0000FFC8;
static constexpr uint32_t XK_F12        = 0x0000FFC9;
static constexpr uint32_t XK_F13        = 0x0000FFCA;
static constexpr uint32_t XK_F14        = 0x0000FFCB;
static constexpr uint32_t XK_F15        = 0x0000FFCC;
static constexpr uint32_t XK_F16        = 0x0000FFCD;
static constexpr uint32_t XK_F17        = 0x0000FFCE;
static constexpr uint32_t XK_F18        = 0x0000FFCF;
static constexpr uint32_t XK_F19        = 0x0000FFD0;
static constexpr uint32_t XK_F20        = 0x0000FFD1;

// Modifiers
static constexpr uint32_t XK_Shift_L    = 0x0000FFE1;
static constexpr uint32_t XK_Shift_R    = 0x0000FFE2;
static constexpr uint32_t XK_Control_L  = 0x0000FFE3;
static constexpr uint32_t XK_Control_R  = 0x0000FFE4;
static constexpr uint32_t XK_Caps_Lock  = 0x0000FFE5;
static constexpr uint32_t XK_Meta_L     = 0x0000FFE7;
static constexpr uint32_t XK_Meta_R     = 0x0000FFE8;
static constexpr uint32_t XK_Alt_L      = 0x0000FFE9;
static constexpr uint32_t XK_Alt_R      = 0x0000FFEA;
static constexpr uint32_t XK_Super_L    = 0x0000FFEB;
static constexpr uint32_t XK_Super_R    = 0x0000FFEC;

// Latin-1 supplement (selected)
static constexpr uint32_t XK_space      = 0x00000020;
static constexpr uint32_t XK_section    = 0x000000A7;
static constexpr uint32_t XK_plusminus  = 0x000000B1;
