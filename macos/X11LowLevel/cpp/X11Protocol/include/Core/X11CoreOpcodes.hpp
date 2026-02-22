//
//  X11CoreOpcodes.hpp
//  X11LowLevel
//
//  Created by Lawrence Gibbons on 2/10/26.
//

#pragma once
#include <cstdint>

namespace x11 {

// ================================================================================================
// Core protocol major opcodes (X11 core)
//
// Keep callsites readable:
//   reg.registerMajor(x11::opcode::QueryPointer, &QueryOps::onMajor, this);
// ================================================================================================
namespace opcode {

// --- Window / hierarchy (1–15) ---
static constexpr uint8_t CreateWindow            =   1;
static constexpr uint8_t ChangeWindowAttributes  =   2;
static constexpr uint8_t GetWindowAttributes     =   3;
static constexpr uint8_t DestroyWindow           =   4;
static constexpr uint8_t DestroySubwindows       =   5;
static constexpr uint8_t ChangeSaveSet           =   6;
static constexpr uint8_t ReparentWindow          =   7;
static constexpr uint8_t MapWindow               =   8;
static constexpr uint8_t MapSubwindows           =   9;
static constexpr uint8_t UnmapWindow             =  10;
static constexpr uint8_t UnmapSubwindows         =  11;
static constexpr uint8_t ConfigureWindow         =  12;
static constexpr uint8_t CirculateWindow         =  13;
static constexpr uint8_t GetGeometry             =  14;
static constexpr uint8_t QueryTree               =  15;

// --- Atoms / properties / selections (16–25) ---
static constexpr uint8_t InternAtom              =  16;
static constexpr uint8_t GetAtomName             =  17;
static constexpr uint8_t ChangeProperty          =  18;
static constexpr uint8_t DeleteProperty          =  19;
static constexpr uint8_t GetProperty             =  20;
static constexpr uint8_t ListProperties          =  21;
static constexpr uint8_t SetSelectionOwner       =  22;
static constexpr uint8_t GetSelectionOwner       =  23;
static constexpr uint8_t ConvertSelection        =  24;
static constexpr uint8_t SendEvent               =  25;

// --- Grabs / pointer / focus (26–44) ---
static constexpr uint8_t GrabPointer             =  26;
static constexpr uint8_t UngrabPointer           =  27;
static constexpr uint8_t GrabButton              =  28;
static constexpr uint8_t UngrabButton            =  29;
static constexpr uint8_t ChangeActivePointerGrab =  30;
static constexpr uint8_t GrabKeyboard            =  31;
static constexpr uint8_t UngrabKeyboard          =  32;
static constexpr uint8_t GrabKey                 =  33;
static constexpr uint8_t UngrabKey               =  34;
static constexpr uint8_t AllowEvents             =  35;
static constexpr uint8_t GrabServer              =  36;
static constexpr uint8_t UngrabServer            =  37;
static constexpr uint8_t QueryPointer            =  38;
static constexpr uint8_t GetMotionEvents         =  39;
static constexpr uint8_t TranslateCoords         =  40;
static constexpr uint8_t WarpPointer             =  41;
static constexpr uint8_t SetInputFocus           =  42;
static constexpr uint8_t GetInputFocus           =  43;
static constexpr uint8_t QueryKeymap             =  44;

// --- Fonts (45–52) ---
static constexpr uint8_t OpenFont                =  45;
static constexpr uint8_t CloseFont               =  46;
static constexpr uint8_t QueryFont               =  47;
static constexpr uint8_t QueryTextExtents        =  48;
static constexpr uint8_t ListFonts               =  49;
static constexpr uint8_t ListFontsWithInfo       =  50;
static constexpr uint8_t SetFontPath             =  51;
static constexpr uint8_t GetFontPath             =  52;

// --- Pixmaps / GCs / drawing (53–77) ---
static constexpr uint8_t CreatePixmap            =  53;
static constexpr uint8_t FreePixmap              =  54;
static constexpr uint8_t CreateGC                =  55;
static constexpr uint8_t ChangeGC                =  56;
static constexpr uint8_t CopyGC                  =  57;
static constexpr uint8_t SetDashes               =  58;
static constexpr uint8_t SetClipRectangles       =  59;
static constexpr uint8_t FreeGC                  =  60;
static constexpr uint8_t ClearArea               =  61;
static constexpr uint8_t CopyArea                =  62;
static constexpr uint8_t CopyPlane               =  63;
static constexpr uint8_t PolyPoint               =  64;
static constexpr uint8_t PolyLine                =  65;
static constexpr uint8_t PolySegment             =  66;
static constexpr uint8_t PolyRectangle           =  67;
static constexpr uint8_t PolyArc                 =  68;
static constexpr uint8_t FillPoly                =  69;
static constexpr uint8_t PolyFillRectangle       =  70;
static constexpr uint8_t PolyFillArc             =  71;
static constexpr uint8_t PutImage                =  72;
static constexpr uint8_t GetImage                =  73;
static constexpr uint8_t PolyText8               =  74;
static constexpr uint8_t PolyText16              =  75;
static constexpr uint8_t ImageText8              =  76;
static constexpr uint8_t ImageText16             =  77;

// --- Colormaps / cursors / extensions (78–99) ---
static constexpr uint8_t CreateColormap          =  78;
static constexpr uint8_t FreeColormap            =  79;
static constexpr uint8_t CopyColormapAndFree     =  80;
static constexpr uint8_t InstallColormap         =  81;
static constexpr uint8_t UninstallColormap       =  82;
static constexpr uint8_t ListInstalledColormaps  =  83;
static constexpr uint8_t AllocColor              =  84;
static constexpr uint8_t AllocNamedColor         =  85;
static constexpr uint8_t AllocColorCells         =  86;
static constexpr uint8_t AllocColorPlanes        =  87;
static constexpr uint8_t FreeColors              =  88;
static constexpr uint8_t StoreColors             =  89;
static constexpr uint8_t StoreNamedColor         =  90;
static constexpr uint8_t QueryColors             =  91;
static constexpr uint8_t LookupColor             =  92;
static constexpr uint8_t CreateCursor            =  93;
static constexpr uint8_t CreateGlyphCursor       =  94;
static constexpr uint8_t FreeCursor              =  95;
static constexpr uint8_t RecolorCursor           =  96;
static constexpr uint8_t QueryBestSize           =  97;
static constexpr uint8_t QueryExtension          =  98;
static constexpr uint8_t ListExtensions          =  99;

// --- Keyboard / pointer mapping / misc (100–127) ---
static constexpr uint8_t ChangeKeyboardMapping   = 100;
static constexpr uint8_t GetKeyboardMapping      = 101;
static constexpr uint8_t ChangeKeyboardControl   = 102;
static constexpr uint8_t GetKeyboardControl      = 103;
static constexpr uint8_t Bell                    = 104;
static constexpr uint8_t ChangePointerControl    = 105;
static constexpr uint8_t GetPointerControl       = 106;
static constexpr uint8_t SetScreenSaver          = 107;
static constexpr uint8_t GetScreenSaver          = 108;
static constexpr uint8_t ChangeHosts             = 109;
static constexpr uint8_t ListHosts               = 110;
static constexpr uint8_t SetAccessControl        = 111;
static constexpr uint8_t SetCloseDownMode        = 112;
static constexpr uint8_t KillClient              = 113;
static constexpr uint8_t RotateProperties        = 114;
static constexpr uint8_t ForceScreenSaver        = 115;
static constexpr uint8_t SetPointerMapping       = 116;
static constexpr uint8_t GetPointerMapping       = 117;
static constexpr uint8_t SetModifierMapping      = 118;
static constexpr uint8_t GetModifierMapping      = 119;
static constexpr uint8_t NoOperation             = 127;

} // namespace opcode

// ================================================================================================
// X11 SelectInput / event-mask helpers (WindowView::event_mask)
//
// Use these instead of magic numbers like (1u << 17).
// ================================================================================================
namespace mask {

static constexpr uint32_t KeyPress             = (1u << 0);
static constexpr uint32_t KeyRelease           = (1u << 1);
static constexpr uint32_t ButtonPress          = (1u << 2);
static constexpr uint32_t ButtonRelease        = (1u << 3);
static constexpr uint32_t EnterWindow          = (1u << 4);
static constexpr uint32_t LeaveWindow          = (1u << 5);
static constexpr uint32_t PointerMotion        = (1u << 6);
static constexpr uint32_t PointerMotionHint    = (1u << 7);
static constexpr uint32_t Button1Motion        = (1u << 8);
static constexpr uint32_t Button2Motion        = (1u << 9);
static constexpr uint32_t Button3Motion        = (1u << 10);
static constexpr uint32_t Button4Motion        = (1u << 11);
static constexpr uint32_t Button5Motion        = (1u << 12);
static constexpr uint32_t ButtonMotion         = (1u << 13);
static constexpr uint32_t KeymapState          = (1u << 14);
static constexpr uint32_t Exposure             = (1u << 15);
static constexpr uint32_t VisibilityChange     = (1u << 16);
static constexpr uint32_t StructureNotify      = (1u << 17);
static constexpr uint32_t ResizeRedirect       = (1u << 18);
static constexpr uint32_t SubstructureNotify   = (1u << 19);
static constexpr uint32_t SubstructureRedirect = (1u << 20);
static constexpr uint32_t FocusChange          = (1u << 21);
static constexpr uint32_t PropertyChange       = (1u << 22);
static constexpr uint32_t ColormapChange       = (1u << 23);
static constexpr uint32_t OwnerGrabButton      = (1u << 24);

} // namespace mask

  
namespace error {
static constexpr uint8_t BadRequest   = 1;
static constexpr uint8_t BadValue     = 2;
static constexpr uint8_t BadWindow    = 3;
static constexpr uint8_t BadPixmap    = 4;
static constexpr uint8_t BadAtom      = 5;
static constexpr uint8_t BadCursor    = 6;
static constexpr uint8_t BadFont      = 7;
static constexpr uint8_t BadMatch     = 8;
static constexpr uint8_t BadDrawable  = 9;
static constexpr uint8_t BadAccess    = 10;
static constexpr uint8_t BadAlloc     = 11;
static constexpr uint8_t BadColor     = 12;
static constexpr uint8_t BadGC        = 13;
static constexpr uint8_t BadIDChoice  = 14;
static constexpr uint8_t BadName      = 15;
static constexpr uint8_t BadLength    = 16;
static constexpr uint8_t BadImplementation = 17;
}
  
} // namespace x11
