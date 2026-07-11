/* Minimal X11 keysym compatibility for the native macOS backend.
 * Values intentionally match X11 so config.h, vimnav.c, and cmdline.c can
 * share the same key tables without linking to XQuartz. */
#ifndef ST_MACOS_KEYSYMS_H
#define ST_MACOS_KEYSYMS_H

#include <limits.h>

typedef unsigned long KeySym;

#define NoSymbol          0L

#define ShiftMask         (1U << 0)
#define LockMask          (1U << 1)
#define ControlMask       (1U << 2)
#define Mod1Mask          (1U << 3)
#define Mod2Mask          (1U << 4)
#define Mod3Mask          (1U << 5)
#define Mod4Mask          (1U << 6)
#define Mod5Mask          (1U << 7)

#define Button1Mask       (1U << 8)
#define Button2Mask       (1U << 9)
#define Button3Mask       (1U << 10)
#define Button4Mask       (1U << 11)
#define Button5Mask       (1U << 12)

#define Button1           1U
#define Button2           2U
#define Button3           3U
#define Button4           4U
#define Button5           5U

#define XK_ANY_MOD        UINT_MAX
#define XK_NO_MOD         0U
#define XK_SWITCH_MOD     ((1U << 13) | (1U << 14))

#define XK_BackSpace      0xff08
#define XK_Tab            0xff09
#define XK_Return         0xff0d
#define XK_Escape         0xff1b
#define XK_Home           0xff50
#define XK_Left           0xff51
#define XK_Up             0xff52
#define XK_Right          0xff53
#define XK_Down           0xff54
#define XK_Prior          0xff55
#define XK_Page_Up        XK_Prior
#define XK_Next           0xff56
#define XK_Page_Down      XK_Next
#define XK_End            0xff57
#define XK_Begin          0xff58
#define XK_Print          0xff61
#define XK_Insert         0xff63
#define XK_Break          0xff6b
#define XK_Num_Lock       0xff7f

#define XK_KP_Space       0xff80
#define XK_KP_Tab         0xff89
#define XK_KP_Enter       0xff8d
#define XK_KP_Home        0xff95
#define XK_KP_Left        0xff96
#define XK_KP_Up          0xff97
#define XK_KP_Right       0xff98
#define XK_KP_Down        0xff99
#define XK_KP_Prior       0xff9a
#define XK_KP_Next        0xff9b
#define XK_KP_End         0xff9c
#define XK_KP_Begin       0xff9d
#define XK_KP_Insert      0xff9e
#define XK_KP_Delete      0xff9f
#define XK_KP_Multiply    0xffaa
#define XK_KP_Add         0xffab
#define XK_KP_Subtract    0xffad
#define XK_KP_Decimal     0xffae
#define XK_KP_Divide      0xffaf
#define XK_KP_0           0xffb0
#define XK_KP_1           0xffb1
#define XK_KP_2           0xffb2
#define XK_KP_3           0xffb3
#define XK_KP_4           0xffb4
#define XK_KP_5           0xffb5
#define XK_KP_6           0xffb6
#define XK_KP_7           0xffb7
#define XK_KP_8           0xffb8
#define XK_KP_9           0xffb9

#define XK_F1             0xffbe
#define XK_F2             0xffbf
#define XK_F3             0xffc0
#define XK_F4             0xffc1
#define XK_F5             0xffc2
#define XK_F6             0xffc3
#define XK_F7             0xffc4
#define XK_F8             0xffc5
#define XK_F9             0xffc6
#define XK_F10            0xffc7
#define XK_F11            0xffc8
#define XK_F12            0xffc9
#define XK_F13            0xffca
#define XK_F14            0xffcb
#define XK_F15            0xffcc
#define XK_F16            0xffcd
#define XK_F17            0xffce
#define XK_F18            0xffcf
#define XK_F19            0xffd0
#define XK_F20            0xffd1
#define XK_F21            0xffd2
#define XK_F22            0xffd3
#define XK_F23            0xffd4
#define XK_F24            0xffd5
#define XK_F25            0xffd6
#define XK_F26            0xffd7
#define XK_F27            0xffd8
#define XK_F28            0xffd9
#define XK_F29            0xffda
#define XK_F30            0xffdb
#define XK_F31            0xffdc
#define XK_F32            0xffdd
#define XK_F33            0xffde
#define XK_F34            0xffdf
#define XK_F35            0xffe0

#define XK_Shift_L        0xffe1
#define XK_Shift_R        0xffe2
#define XK_Control_L      0xffe3
#define XK_Control_R      0xffe4
#define XK_Caps_Lock      0xffe5
#define XK_Meta_L         0xffe7
#define XK_Meta_R         0xffe8
#define XK_Alt_L          0xffe9
#define XK_Alt_R          0xffea
#define XK_Super_L        0xffeb
#define XK_Super_R        0xffec
#define XK_Delete         0xffff
#define XK_ISO_Left_Tab   0xfe20

#define XK_space          0x20
#define XK_0              '0'
#define XK_1              '1'
#define XK_2              '2'
#define XK_3              '3'
#define XK_4              '4'
#define XK_5              '5'
#define XK_6              '6'
#define XK_7              '7'
#define XK_8              '8'
#define XK_9              '9'
#define XK_C              'C'
#define XK_E              'E'
#define XK_O              'O'
#define XK_V              'V'
#define XK_Y              'Y'
#define XK_m              'm'
#define XK_equal          '='
#define XK_minus          '-'
#define XK_semicolon      ';'
#define XK_bracketleft    '['
#define XK_bracketright   ']'

#define XC_hand2          60U
#define XC_left_ptr       68U
#define XC_xterm          152U

#define IsModifierKey(k) ((k) >= XK_Shift_L && (k) <= XK_Super_R)

#endif
