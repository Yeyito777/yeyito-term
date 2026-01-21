/* See LICENSE for license details. */
/* Mock declarations for st unit tests */

#ifndef MOCKS_H
#define MOCKS_H

#include "../st.h"

/* Terminal mode flags from st.c */
enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_INSERT      = 1 << 1,
	MODE_ALTSCREEN   = 1 << 2,
	MODE_CRLF        = 1 << 3,
	MODE_ECHO        = 1 << 4,
	MODE_PRINT       = 1 << 5,
	MODE_UTF8        = 1 << 6,
};

/* Cursor structure */
typedef struct {
	Glyph attr;
	int x;
	int y;
	char state;
} TCursor;

/* Selection structure */
typedef struct {
	int mode;
	int type;
	int snap;
	struct {
		int x, y;
	} nb, ne, ob, oe;
	int alt;
} Selection;

/* Terminal structure (minimal for testing) */
#define HISTSIZE (1 << 15)
typedef struct {
	int row;
	int col;
	int maxcol;
	Line *line;
	Line *alt;
	Line hist[HISTSIZE];
	int histi;
	int scr;
	int *dirty;
	TCursor c;
	int ocx;
	int ocy;
	int top;
	int bot;
	int mode;
	int esc;
	char trantbl[4];
	int charset;
	int icharset;
	int *tabs;
	Rune lastc;
} Term;

/* Mock globals */
extern Term term;
extern Selection sel;
extern wchar_t *worddelimiters;

/* Mock function call tracking */
typedef struct {
	int tfulldirt_calls;
	int selclear_calls;
	int selstart_calls;
	int selextend_calls;
	int kscrollup_calls;
	int kscrolldown_calls;
	int xsetsel_calls;
	int xclipcopy_calls;
	int clippaste_calls;
	int ttywrite_calls;

	/* Last arguments */
	struct { int x, y, snap; } last_selstart;
	struct { int x, y, type, done; } last_selextend;
	struct { int n; } last_kscrollup;
	struct { int n; } last_kscrolldown;
	char *last_xsetsel;
	char ttywrite_buf[256];  /* Buffer of keys sent via ttywrite */
	int ttywrite_buf_len;
} MockState;

extern MockState mock_state;

/* Reset all mocks */
void mock_reset(void);

/* Initialize terminal for testing */
void mock_term_init(int rows, int cols);
void mock_term_free(void);

/* Set line content for testing */
void mock_set_line(int y, const char *content);

/*
 * X11/Xft mock types and functions for sshind testing
 * These allow testing sshind logic without a real X server
 */

/* Mock X11 types */
typedef unsigned long XID;
typedef XID Window;
typedef XID Drawable;
typedef XID Colormap;
typedef struct _XDisplay Display;
typedef struct _Visual Visual;
typedef struct _XGC *GC;
typedef int Bool;

/* Mock Xft types */
typedef struct {
	unsigned long pixel;
} XftColor;

typedef struct {
	int ascent;
	int descent;
} XftFont;

typedef struct _XftDraw XftDraw;

/* Mock X11 globals for sshind */
typedef struct {
	Display *dpy;
	Colormap cmap;
	Window win;
	Drawable buf;
	Visual *vis;
	int scr;
} MockXWindow;

typedef struct {
	int tw, th;
	int w, h;
	int ch, cw;
	int mode;
	int cursor;
} MockTermWindow;

typedef struct {
	XftColor *col;
	size_t collen;
	void *font, *bfont, *ifont, *ibfont;
	GC gc;
} MockDC;

extern MockXWindow mock_xw;
extern MockTermWindow mock_win;
extern MockDC mock_dc;
extern char *mock_usedfont;
extern double mock_usedfontsize;

/* X11 mock call tracking */
typedef struct {
	int xcreatepixmap_calls;
	int xfreepixmap_calls;
	int xcreatewindow_calls;
	int xdestroywindow_calls;
	int xcreategc_calls;
	int xfreegc_calls;
	int xcopyarea_calls;
	GC last_xcopyarea_gc;  /* Track which GC was used */
} X11MockState;

extern X11MockState x11_mock_state;

/* Reset X11 mocks */
void x11_mock_reset(void);

/* Initialize X11 mocks for sshind testing */
void x11_mock_init(void);

#endif /* MOCKS_H */
