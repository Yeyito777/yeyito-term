/* See LICENSE for license details. */
#include "clipboard5522.h"

enum win_mode {
	MODE_VISIBLE     = 1 << 0,
	MODE_FOCUSED     = 1 << 1,
	MODE_APPKEYPAD   = 1 << 2,
	MODE_MOUSEBTN    = 1 << 3,
	MODE_MOUSEMOTION = 1 << 4,
	MODE_REVERSE     = 1 << 5,
	MODE_KBDLOCK     = 1 << 6,
	MODE_HIDE        = 1 << 7,
	MODE_APPCURSOR   = 1 << 8,
	MODE_MOUSESGR    = 1 << 9,
	MODE_8BIT        = 1 << 10,
	MODE_KITTYKBD    = 1 << 18,
	MODE_BLINK       = 1 << 11,
	MODE_FBLINK      = 1 << 12,
	MODE_FOCUS       = 1 << 13,
	MODE_MOUSEX10    = 1 << 14,
	MODE_MOUSEMANY   = 1 << 15,
	MODE_BRCKTPASTE  = 1 << 16,
	MODE_NUMLOCK     = 1 << 17,
	MODE_PASTEEVENT  = 1 << 19,
	MODE_MOUSE       = MODE_MOUSEBTN|MODE_MOUSEMOTION|MODE_MOUSEX10\
	                  |MODE_MOUSEMANY,
};

/* Modes selected by terminal applications rather than by the window system.
 * Clear these when an application boundary is lost (for example, when SSH
 * disconnects before a remote TUI can emit its normal teardown sequences). */
#define MODE_APPRESET (MODE_APPKEYPAD | MODE_MOUSE | MODE_REVERSE | \
	MODE_KBDLOCK | MODE_HIDE | MODE_APPCURSOR | MODE_MOUSESGR | MODE_8BIT | \
	MODE_KITTYKBD | MODE_FOCUS | MODE_BRCKTPASTE | MODE_PASTEEVENT)

static inline unsigned int
winmoderestore(unsigned int mode)
{
	return mode & ~MODE_APPRESET;
}

void xbell(void);
void xcleanup(void);
void xclipcopy(void);
void clippaste(const Arg *);
void xdrawcursor(int, int, Glyph, int, int, Glyph);
void xdrawline(Line, int, int, int);
void xfinishdraw(void);
void xloadcols(void);
int xsetcolorname(int, const char *);
int xgetcolor(int, unsigned char *, unsigned char *, unsigned char *);
void xseticontitle(char *);
void xsettitle(char *);
void xsetcwd(char *);
int xgetcursor(void);
int xsetcursor(int);
int xgpuactive(void);
void xsetmode(int, unsigned int);
int xismode(unsigned int);
void xresetmode(void);
int xgpuenabled(void);
void xsetgraphicsmode(int);
int xdrawrowtop(int);
int xdrawrowbottom(int);
void xsetpointermotion(int);
void xsetmousecursor(int);
void xsetsel(char *);
int xstartdraw(void);
void xximspot(int, int);
void xsetdwmsaveargv(const char *);
#ifndef ST_NATIVE_MACOS
void xclip5522read(const Clip5522Request *);
#endif
