/* See LICENSE for license details. */
#include <stdio.h>
#include <string.h>

#ifndef SSHIND_TEST
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#endif

#include "sshind.h"

#ifndef SSHIND_TEST
typedef XftDraw *Draw;
typedef XftColor Color;

/* Purely graphic info */
typedef struct {
	int tw, th; /* tty width and height */
	int w, h; /* window width and height */
	int ch; /* char height */
	int cw; /* char width  */
	int mode; /* window state/mode flags */
	int cursor; /* cursor style */
} TermWindow;

typedef struct {
	Display *dpy;
	Colormap cmap;
	Window win;
	Drawable buf;
	void *specbuf; /* font spec buffer used for rendering */
	Atom xembed, wmdeletewin, netwmname, netwmiconname, netwmpid, stcwd, stnotify;
	struct {
		XIM xim;
		XIC xic;
		XPoint spot;
		XVaNestedList spotlist;
	} ime;
	Draw draw;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	int isfixed; /* is fixed geometry? */
	int l, t; /* left and top offset */
	int gm; /* geometry mask */
} XWindow;

/* Drawing Context */
typedef struct {
	Color *col;
	size_t collen;
	void *font, *bfont, *ifont, *ibfont;  /* Font structs not needed for sshind */
	GC gc;
} DC;

/* Extern declarations for x.c globals */
extern XWindow xw;
extern TermWindow win;
extern DC dc;
extern char *usedfont;
extern double usedfontsize;
#endif /* !SSHIND_TEST */

/* SSH indicator overlay state */
static struct {
	int active;              /* whether indicator is shown */
	char host[256];          /* hostname being displayed */
	Window win;              /* X window for the overlay */
	Drawable buf;            /* off-screen buffer */
	XftDraw *draw;           /* Xft drawing context */
	XftFont *font;           /* larger font for indicator */
	XftColor fg, bg, border; /* colors */
	int width, height;       /* window dimensions */
	GC gc;                   /* graphics context for this window */
} sshind;

int
sshind_active(void)
{
	return sshind.active;
}

int
sshind_height(void)
{
	if (!sshind.active)
		return 0;
	return sshind.height + 2 * sshind_border_width;
}

void
sshind_show(const char *host)
{
	FcPattern *pattern;
	XSetWindowAttributes attrs;
	XGlyphInfo extents;
	double fontsize;
	int x, y;

	/* Store the hostname */
	strncpy(sshind.host, host, sizeof(sshind.host) - 1);
	sshind.host[sizeof(sshind.host) - 1] = '\0';

	/* Load a larger font for the indicator */
	fontsize = usedfontsize * sshind_font_scale;
	pattern = FcNameParse((const FcChar8 *)usedfont);
	if (!pattern) {
		fprintf(stderr, "sshind: can't parse font pattern\n");
		return;
	}
	FcPatternDel(pattern, FC_PIXEL_SIZE);
	FcPatternDel(pattern, FC_SIZE);
	FcPatternAddDouble(pattern, FC_PIXEL_SIZE, fontsize);
	FcConfigSubstitute(NULL, pattern, FcMatchPattern);
	XftDefaultSubstitute(xw.dpy, xw.scr, pattern);

	FcResult result;
	FcPattern *match = FcFontMatch(NULL, pattern, &result);
	if (!match) {
		FcPatternDestroy(pattern);
		fprintf(stderr, "sshind: can't match font\n");
		return;
	}

	sshind.font = XftFontOpenPattern(xw.dpy, match);
	if (!sshind.font) {
		FcPatternDestroy(pattern);
		FcPatternDestroy(match);
		fprintf(stderr, "sshind: can't open font\n");
		return;
	}
	FcPatternDestroy(pattern);

	/* Calculate text dimensions */
	XftTextExtentsUtf8(xw.dpy, sshind.font, (const FcChar8 *)sshind.host,
	                   strlen(sshind.host), &extents);

	sshind.width = extents.xOff + 2 * sshind_padding + 2 * sshind_border_width;
	sshind.height = sshind.font->ascent + sshind.font->descent +
	                2 * sshind_padding + 2 * sshind_border_width;

	/* Allocate colors */
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, sshind_fg_color, &sshind.fg);
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, sshind_bg_color, &sshind.bg);
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, sshind_border_color, &sshind.border);

	/* Calculate position (top-right with margin) */
	x = win.w - sshind.width - sshind_margin;
	y = sshind_margin;

	/* Create the overlay window */
	attrs.background_pixel = sshind.bg.pixel;
	attrs.border_pixel = sshind.border.pixel;
	attrs.override_redirect = True;  /* Don't let WM manage this window */
	attrs.event_mask = ExposureMask;

	sshind.win = XCreateWindow(xw.dpy, xw.win, x, y,
	                           sshind.width, sshind.height,
	                           sshind_border_width,
	                           XDefaultDepth(xw.dpy, xw.scr),
	                           InputOutput, xw.vis,
	                           CWBackPixel | CWBorderPixel |
	                           CWOverrideRedirect | CWEventMask,
	                           &attrs);

	/* Create off-screen buffer for double buffering */
	sshind.buf = XCreatePixmap(xw.dpy, sshind.win, sshind.width, sshind.height,
	                           DefaultDepth(xw.dpy, xw.scr));

	/* Create Xft drawing context */
	sshind.draw = XftDrawCreate(xw.dpy, sshind.buf, xw.vis, xw.cmap);

	/* Create GC for this window */
	XGCValues gcvalues;
	gcvalues.graphics_exposures = False;
	sshind.gc = XCreateGC(xw.dpy, sshind.win, GCGraphicsExposures, &gcvalues);

	/* Map the window */
	XMapRaised(xw.dpy, sshind.win);

	sshind.active = 1;

	/* Initial draw */
	sshind_draw();
}

void
sshind_hide(void)
{
	if (!sshind.active)
		return;

	/* Destroy resources */
	if (sshind.draw) {
		XftDrawDestroy(sshind.draw);
		sshind.draw = NULL;
	}
	if (sshind.buf) {
		XFreePixmap(xw.dpy, sshind.buf);
		sshind.buf = 0;
	}
	if (sshind.win) {
		XDestroyWindow(xw.dpy, sshind.win);
		sshind.win = 0;
	}
	if (sshind.font) {
		XftFontClose(xw.dpy, sshind.font);
		sshind.font = NULL;
	}
	if (sshind.gc) {
		XFreeGC(xw.dpy, sshind.gc);
		sshind.gc = 0;
	}

	/* Free colors */
	XftColorFree(xw.dpy, xw.vis, xw.cmap, &sshind.fg);
	XftColorFree(xw.dpy, xw.vis, xw.cmap, &sshind.bg);
	XftColorFree(xw.dpy, xw.vis, xw.cmap, &sshind.border);

	sshind.active = 0;
	sshind.host[0] = '\0';
}

void
sshind_draw(void)
{
	int tx, ty;

	if (!sshind.active || !sshind.draw)
		return;

	/* Clear background */
	XftDrawRect(sshind.draw, &sshind.bg, 0, 0, sshind.width, sshind.height);

	/* Draw text centered */
	tx = sshind_padding + sshind_border_width;
	ty = sshind_padding + sshind_border_width + sshind.font->ascent;

	XftDrawStringUtf8(sshind.draw, &sshind.fg, sshind.font, tx, ty,
	                  (const FcChar8 *)sshind.host, strlen(sshind.host));

	/* Copy buffer to window */
	XCopyArea(xw.dpy, sshind.buf, sshind.win, sshind.gc,
	          0, 0, sshind.width, sshind.height, 0, 0);
}

void
sshind_resize(void)
{
	int x, y;

	if (!sshind.active)
		return;

	/* Recalculate position (top-right with margin) */
	x = win.w - sshind.width - sshind_margin;
	y = sshind_margin;

	/* Move the window */
	XMoveWindow(xw.dpy, sshind.win, x, y);
}
