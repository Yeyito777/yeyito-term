/* See LICENSE for license details. */
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef NOTIF_TEST
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#endif

#include "sshind.h"
#include "notif.h"

#ifndef NOTIF_TEST
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
	void *font, *bfont, *ifont, *ibfont;  /* Font structs not needed for notif */
	GC gc;
} DC;

/* Extern declarations for x.c globals */
extern XWindow xw;
extern TermWindow win;
extern DC dc;
extern char *usedfont;
extern double usedfontsize;
extern int debug_mode;
#endif /* !NOTIF_TEST */

/* Notification overlay state */
static struct {
	int active;              /* whether notification is shown */
	char msg[512];           /* message being displayed */
	Window win;              /* X window for the overlay */
	Drawable buf;            /* off-screen buffer */
	XftDraw *draw;           /* Xft drawing context */
	XftFont *font;           /* larger font for notification */
	XftColor fg, bg, border; /* colors */
	int width, height;       /* window dimensions */
	GC gc;                   /* graphics context for this window */
	struct timespec show_time; /* when the notification was shown */
} notif;

int
notif_active(void)
{
	return notif.active;
}

int
notif_check_timeout(struct timespec *now)
{
	double elapsed;

	if (!notif.active)
		return -1;

	elapsed = TIMEDIFF((*now), notif.show_time);
	return (int)(notif_display_ms - elapsed);
}

void
notif_show(const char *msg)
{
	FcPattern *pattern;
	XSetWindowAttributes attrs;
	XGlyphInfo extents;
	double fontsize;
	int x, y;

	/* If already active, clean up first */
	if (notif.active)
		notif_hide();

	/* Store the message */
	strncpy(notif.msg, msg, sizeof(notif.msg) - 1);
	notif.msg[sizeof(notif.msg) - 1] = '\0';

	if (debug_mode)
		fprintf(stderr, "notif: showing \"%s\"\n", notif.msg);

	/* Load a larger font for the notification */
	fontsize = usedfontsize * notif_font_scale;
	pattern = FcNameParse((const FcChar8 *)usedfont);
	if (!pattern) {
		fprintf(stderr, "notif: can't parse font pattern\n");
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
		fprintf(stderr, "notif: can't match font\n");
		return;
	}

	notif.font = XftFontOpenPattern(xw.dpy, match);
	if (!notif.font) {
		FcPatternDestroy(pattern);
		FcPatternDestroy(match);
		fprintf(stderr, "notif: can't open font\n");
		return;
	}
	FcPatternDestroy(pattern);

	/* Calculate text dimensions */
	XftTextExtentsUtf8(xw.dpy, notif.font, (const FcChar8 *)notif.msg,
	                   strlen(notif.msg), &extents);

	notif.width = extents.xOff + 2 * notif_padding + 2 * notif_border_width;
	notif.height = notif.font->ascent + notif.font->descent +
	                2 * notif_padding + 2 * notif_border_width;

	/* Allocate colors */
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, notif_fg_color, &notif.fg);
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, notif_bg_color, &notif.bg);
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, notif_border_color, &notif.border);

	/* Calculate position (top-right with margin, below sshind if active) */
	x = win.w - notif.width - notif_margin;
	y = notif_margin;
	if (sshind_active())
		y += sshind_height() + notif_margin;

	/* Create the overlay window as a child of the main st window */
	attrs.background_pixel = notif.bg.pixel;
	attrs.border_pixel = notif.border.pixel;
	attrs.override_redirect = True;  /* Don't let WM manage this window */
	attrs.event_mask = ExposureMask;
	attrs.colormap = xw.cmap;

	notif.win = XCreateWindow(xw.dpy, xw.win, x, y,
	                           notif.width, notif.height,
	                           notif_border_width,
	                           XDefaultDepth(xw.dpy, xw.scr),
	                           InputOutput, xw.vis,
	                           CWBackPixel | CWBorderPixel |
	                           CWOverrideRedirect | CWEventMask |
	                           CWColormap,
	                           &attrs);

	/* Create off-screen buffer for double buffering */
	notif.buf = XCreatePixmap(xw.dpy, notif.win, notif.width, notif.height,
	                           XDefaultDepth(xw.dpy, xw.scr));

	/* Create Xft drawing context */
	notif.draw = XftDrawCreate(xw.dpy, notif.buf, xw.vis, xw.cmap);

	/* Create GC for this window */
	XGCValues gcvalues;
	gcvalues.graphics_exposures = False;
	notif.gc = XCreateGC(xw.dpy, notif.win, GCGraphicsExposures, &gcvalues);

	/* Map the window */
	XMapRaised(xw.dpy, notif.win);

	/* Record show time for timeout */
	clock_gettime(CLOCK_MONOTONIC, &notif.show_time);

	notif.active = 1;

	/* Initial draw */
	notif_draw();
}

void
notif_hide(void)
{
	if (!notif.active)
		return;

	if (debug_mode)
		fprintf(stderr, "notif: hiding \"%s\"\n", notif.msg);

	/* Destroy resources */
	if (notif.draw) {
		XftDrawDestroy(notif.draw);
		notif.draw = NULL;
	}
	if (notif.buf) {
		XFreePixmap(xw.dpy, notif.buf);
		notif.buf = 0;
	}
	if (notif.win) {
		XDestroyWindow(xw.dpy, notif.win);
		notif.win = 0;
	}
	if (notif.font) {
		XftFontClose(xw.dpy, notif.font);
		notif.font = NULL;
	}
	if (notif.gc) {
		XFreeGC(xw.dpy, notif.gc);
		notif.gc = 0;
	}

	/* Free colors */
	XftColorFree(xw.dpy, xw.vis, xw.cmap, &notif.fg);
	XftColorFree(xw.dpy, xw.vis, xw.cmap, &notif.bg);
	XftColorFree(xw.dpy, xw.vis, xw.cmap, &notif.border);

	notif.active = 0;
	notif.msg[0] = '\0';
}

void
notif_draw(void)
{
	int tx, ty;

	if (!notif.active || !notif.draw)
		return;

	/* Clear background */
	XftDrawRect(notif.draw, &notif.bg, 0, 0, notif.width, notif.height);

	/* Draw text centered */
	tx = notif_padding + notif_border_width;
	ty = notif_padding + notif_border_width + notif.font->ascent;

	XftDrawStringUtf8(notif.draw, &notif.fg, notif.font, tx, ty,
	                  (const FcChar8 *)notif.msg, strlen(notif.msg));

	/* Copy buffer to window */
	XCopyArea(xw.dpy, notif.buf, notif.win, notif.gc,
	          0, 0, notif.width, notif.height, 0, 0);
}

void
notif_resize(void)
{
	int x, y;

	if (!notif.active)
		return;

	/* Recalculate position (top-right with margin, below sshind if active) */
	x = win.w - notif.width - notif_margin;
	y = notif_margin;
	if (sshind_active())
		y += sshind_height() + notif_margin;

	/* Move the window */
	XMoveWindow(xw.dpy, notif.win, x, y);
}
