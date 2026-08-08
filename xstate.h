/* See LICENSE for license details. */
/* Shared X11 backend state used by the terminal and its child overlays. */

#ifndef XSTATE_H
#define XSTATE_H

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

typedef struct {
	int tw, th; /* tty width and height */
	int w, h; /* window width and height */
	int ch; /* char height */
	int cw; /* char width */
	int mode; /* window state/mode flags */
	int cursor; /* cursor style */
} TermWindow;

/* Keep this definition in one place.  Overlay modules access fields after the
 * Atom list, so private copies silently corrupt their view whenever x.c adds an
 * Atom (as happened when clip5522 was introduced). */
typedef struct {
	Display *dpy;
	Colormap cmap;
	Window win;
	Drawable buf;
	XftGlyphFontSpec *specbuf;
	Atom xembed, wmdeletewin, netwmname, netwmiconname, netwmpid;
	Atom stcwd, stnotify, stsavecmd, clip5522;
	struct {
		XIM xim;
		XIC xic;
		XPoint spot;
		XVaNestedList spotlist;
	} ime;
	XftDraw *draw;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	int isfixed;
	int l, t;
	int gm;
} XWindow;

extern XWindow xw;
extern TermWindow win;
extern char *usedfont;
extern double usedfontsize;

#endif /* XSTATE_H */
