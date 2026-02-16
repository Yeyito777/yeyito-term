/* See LICENSE for license details. */
/* Command-line mode for st terminal (vim-like : command) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>

#include "cmdline.h"
#include "vimnav.h"

typedef XftDraw *Draw;
typedef XftColor Color;

/* Replicate structs from x.c for extern access */
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
	void *specbuf;
	Atom xembed, wmdeletewin, netwmname, netwmiconname, netwmpid, stcwd, stnotify, stsavecmd;
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
	int isfixed;
	int l, t;
	int gm;
} XWindow;

typedef struct {
	Color *col;
	size_t collen;
	void *font, *bfont, *ifont, *ibfont;
	GC gc;
} DC;

extern XWindow xw;
extern TermWindow win;
extern DC dc;
extern char *usedfont;
extern double usedfontsize;
extern int debug_mode;
extern int tisaltscreen(void);

/* Command-line states */
enum {
	CMDLINE_HIDDEN = 0,
	CMDLINE_INPUT,
	CMDLINE_ERROR,
};

static struct {
	Window win;
	Drawable buf;
	XftDraw *draw;
	GC gc;
	XftFont *font;
	XftColor fg, bg, err, curcolor, border;
	int state;
	char input[CMDLINE_MAX_INPUT];
	int input_len;
	int cursor;   /* byte offset of cursor within input */
	char errmsg[512];
	int width, height;
	int y;        /* y position in parent */
	int loaded;
} cl;

/* Open font at terminal's native pixel size */
static XftFont *
cmdline_load_font(void)
{
	FcPattern *pattern, *match;
	FcResult result;
	XftFont *f;

	pattern = FcNameParse((const FcChar8 *)usedfont);
	if (!pattern)
		return NULL;

	FcPatternDel(pattern, FC_PIXEL_SIZE);
	FcPatternDel(pattern, FC_SIZE);
	FcPatternAddDouble(pattern, FC_PIXEL_SIZE, usedfontsize);
	FcConfigSubstitute(NULL, pattern, FcMatchPattern);
	XftDefaultSubstitute(xw.dpy, xw.scr, pattern);

	match = FcFontMatch(NULL, pattern, &result);
	FcPatternDestroy(pattern);
	if (!match)
		return NULL;

	f = XftFontOpenPattern(xw.dpy, match);
	if (!f)
		FcPatternDestroy(match);
	return f;
}

static int
cmdline_load_resources(void)
{
	if (cl.loaded)
		return 1;

	cl.font = cmdline_load_font();
	if (!cl.font) {
		fprintf(stderr, "cmdline: can't open font\n");
		return 0;
	}

	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, cmdline_fg_color, &cl.fg);
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, cmdline_bg_color, &cl.bg);
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, cmdline_err_color, &cl.err);
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, cmdline_cursor_color, &cl.curcolor);
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, cmdline_border_color, &cl.border);

	cl.loaded = 1;
	return 1;
}

static void
cmdline_compute_geometry(void)
{
	int bpx = (win.h - win.th) / 2;

	cl.y = bpx + win.th - win.ch;
	cl.width = win.w;
	cl.height = win.h - cl.y;
}

void
cmdline_init(void)
{
	XSetWindowAttributes attrs;
	XGCValues gcvalues;

	if (!cmdline_load_resources())
		return;

	cmdline_compute_geometry();

	attrs.background_pixel = cl.bg.pixel;
	attrs.border_pixel = cl.bg.pixel;
	attrs.override_redirect = True;
	attrs.event_mask = ExposureMask;
	attrs.colormap = xw.cmap;

	cl.win = XCreateWindow(xw.dpy, xw.win, 0, cl.y,
	                        cl.width, cl.height,
	                        0,
	                        XDefaultDepth(xw.dpy, xw.scr),
	                        InputOutput, xw.vis,
	                        CWBackPixel | CWBorderPixel |
	                        CWOverrideRedirect | CWEventMask |
	                        CWColormap,
	                        &attrs);

	cl.buf = XCreatePixmap(xw.dpy, cl.win, cl.width, cl.height,
	                        XDefaultDepth(xw.dpy, xw.scr));

	cl.draw = XftDrawCreate(xw.dpy, cl.buf, xw.vis, xw.cmap);

	gcvalues.graphics_exposures = False;
	cl.gc = XCreateGC(xw.dpy, cl.win, GCGraphicsExposures, &gcvalues);

	cl.state = CMDLINE_HIDDEN;

	if (debug_mode)
		fprintf(stderr, "cmdline: initialized (y=%d, %dx%d)\n",
		        cl.y, cl.width, cl.height);
}

static void
cmdline_redraw(void)
{
	int tx, ty;
	int cursor_x;
	XGlyphInfo extents;

	if (!cl.draw || !cl.font)
		return;

	/* Clear background */
	XftDrawRect(cl.draw, &cl.bg, 0, 0, cl.width, cl.height);

	/* Draw top border line */
	XftDrawRect(cl.draw, &cl.border, 0, 0, cl.width, cmdline_border_top);

	ty = cmdline_border_top + cl.font->ascent;
	tx = win.cw / 2;

	if (cl.state == CMDLINE_INPUT) {
		/* Draw ":" prefix */
		XftDrawStringUtf8(cl.draw, &cl.fg, cl.font, tx, ty,
		                  (const FcChar8 *)":", 1);
		tx += win.cw;

		/* Draw input text */
		if (cl.input_len > 0) {
			XftDrawStringUtf8(cl.draw, &cl.fg, cl.font, tx, ty,
			                  (const FcChar8 *)cl.input, cl.input_len);
		}

		/* Draw cursor (block) at cursor position */
		cursor_x = tx;
		if (cl.cursor > 0) {
			XftTextExtentsUtf8(xw.dpy, cl.font,
			                   (const FcChar8 *)cl.input,
			                   cl.cursor, &extents);
			cursor_x += extents.xOff;
		}
		XftDrawRect(cl.draw, &cl.curcolor,
		            cursor_x, cmdline_border_top,
		            win.cw, cl.font->ascent + cl.font->descent);

		/* Draw character under cursor in inverted color */
		if (cl.cursor < cl.input_len) {
			int charlen = 1;
			while (cl.cursor + charlen < cl.input_len &&
			       (cl.input[cl.cursor + charlen] & 0xC0) == 0x80)
				charlen++;
			XftDrawStringUtf8(cl.draw, &cl.bg, cl.font,
			                  cursor_x, ty,
			                  (const FcChar8 *)cl.input + cl.cursor,
			                  charlen);
		}
	} else if (cl.state == CMDLINE_ERROR) {
		/* Draw error message in error color */
		XftDrawStringUtf8(cl.draw, &cl.err, cl.font, tx, ty,
		                  (const FcChar8 *)cl.errmsg,
		                  strlen(cl.errmsg));
	}

	/* Copy buffer to window */
	XCopyArea(xw.dpy, cl.buf, cl.win, cl.gc,
	          0, 0, cl.width, cl.height, 0, 0);
}

void
cmdline_open(void)
{
	if (tisaltscreen() && !vimnav.forced)
		return;

	if (cl.state != CMDLINE_HIDDEN) {
		if (cl.state == CMDLINE_ERROR) {
			cl.state = CMDLINE_INPUT;
			cl.input[0] = '\0';
			cl.input_len = 0;
			cl.cursor = 0;
			cmdline_redraw();
		}
		return;
	}

	cl.state = CMDLINE_INPUT;
	cl.input[0] = '\0';
	cl.input_len = 0;
	cl.cursor = 0;

	XMapRaised(xw.dpy, cl.win);
	cmdline_redraw();

	if (debug_mode)
		fprintf(stderr, "cmdline: opened\n");
}

void
cmdline_close(void)
{
	if (cl.state == CMDLINE_HIDDEN)
		return;

	cl.state = CMDLINE_HIDDEN;
	XUnmapWindow(xw.dpy, cl.win);

	if (debug_mode)
		fprintf(stderr, "cmdline: closed\n");
}

static void
cmdline_execute(void)
{
	if (cl.input_len == 0) {
		cmdline_close();
		return;
	}

	if (debug_mode)
		fprintf(stderr, "cmdline: execute '%s'\n", cl.input);

	/* No commands implemented yet */
	snprintf(cl.errmsg, sizeof(cl.errmsg),
	         "Not a terminal command: '%s'", cl.input);
	cl.state = CMDLINE_ERROR;
	cmdline_redraw();
}

int
cmdline_handle_key(unsigned long ksym, unsigned int state,
                   const char *buf, int len)
{
	(void)state;

	/* In error state: any key dismisses */
	if (cl.state == CMDLINE_ERROR) {
		if (debug_mode)
			fprintf(stderr, "cmdline: error dismissed by keypress\n");
		cmdline_close();
		return 1;
	}

	if (cl.state != CMDLINE_INPUT)
		return 0;

	switch (ksym) {
	case XK_Escape:
		cmdline_close();
		return 1;
	case XK_Return:
	case XK_KP_Enter:
		cmdline_execute();
		return 1;
	case XK_Left:
		if (cl.cursor > 0) {
			/* Skip back one UTF-8 character */
			cl.cursor--;
			while (cl.cursor > 0 &&
			       (cl.input[cl.cursor] & 0xC0) == 0x80)
				cl.cursor--;
			cmdline_redraw();
		}
		return 1;
	case XK_Right:
		if (cl.cursor < cl.input_len) {
			/* Skip forward one UTF-8 character */
			cl.cursor++;
			while (cl.cursor < cl.input_len &&
			       (cl.input[cl.cursor] & 0xC0) == 0x80)
				cl.cursor++;
			cmdline_redraw();
		}
		return 1;
	case XK_Home:
		if (cl.cursor > 0) {
			cl.cursor = 0;
			cmdline_redraw();
		}
		return 1;
	case XK_End:
		if (cl.cursor < cl.input_len) {
			cl.cursor = cl.input_len;
			cmdline_redraw();
		}
		return 1;
	case XK_Delete:
		if (cl.cursor < cl.input_len) {
			/* Find end of UTF-8 char at cursor */
			int next = cl.cursor + 1;
			while (next < cl.input_len &&
			       (cl.input[next] & 0xC0) == 0x80)
				next++;
			memmove(cl.input + cl.cursor, cl.input + next,
			        cl.input_len - next);
			cl.input_len -= (next - cl.cursor);
			cl.input[cl.input_len] = '\0';
			cmdline_redraw();
		}
		return 1;
	case XK_BackSpace:
		if (cl.cursor > 0) {
			/* Find start of previous UTF-8 character */
			int prev = cl.cursor - 1;
			while (prev > 0 && (cl.input[prev] & 0xC0) == 0x80)
				prev--;
			memmove(cl.input + prev, cl.input + cl.cursor,
			        cl.input_len - cl.cursor);
			cl.input_len -= (cl.cursor - prev);
			cl.cursor = prev;
			cl.input[cl.input_len] = '\0';
			if (debug_mode)
				fprintf(stderr, "cmdline: backspace, input='%s'\n",
				        cl.input);
			cmdline_redraw();
		} else if (cl.input_len == 0) {
			/* Empty input + backspace = close (like vim) */
			cmdline_close();
		}
		return 1;
	default:
		/* Insert printable characters at cursor */
		if (len > 0 && (unsigned char)buf[0] >= 0x20 &&
		    cl.input_len + len < CMDLINE_MAX_INPUT - 1) {
			memmove(cl.input + cl.cursor + len,
			        cl.input + cl.cursor,
			        cl.input_len - cl.cursor);
			memcpy(cl.input + cl.cursor, buf, len);
			cl.input_len += len;
			cl.cursor += len;
			cl.input[cl.input_len] = '\0';
			if (debug_mode)
				fprintf(stderr, "cmdline: input='%s' cursor=%d\n",
				        cl.input, cl.cursor);
			cmdline_redraw();
		}
		return 1;  /* Consume all keys when active */
	}
}

void
cmdline_draw(void)
{
	if (cl.state != CMDLINE_HIDDEN)
		cmdline_redraw();
}

void
cmdline_resize(void)
{
	int old_y = cl.y;
	int old_w = cl.width;
	int old_h = cl.height;

	cmdline_compute_geometry();

	if (cl.y != old_y || cl.width != old_w || cl.height != old_h) {
		XMoveResizeWindow(xw.dpy, cl.win, 0, cl.y, cl.width, cl.height);

		if (cl.draw)
			XftDrawDestroy(cl.draw);
		if (cl.buf)
			XFreePixmap(xw.dpy, cl.buf);

		cl.buf = XCreatePixmap(xw.dpy, cl.win, cl.width, cl.height,
		                        XDefaultDepth(xw.dpy, xw.scr));
		cl.draw = XftDrawCreate(xw.dpy, cl.buf, xw.vis, xw.cmap);

		if (debug_mode)
			fprintf(stderr, "cmdline: resized (y=%d, %dx%d)\n",
			        cl.y, cl.width, cl.height);
	}

	if (cl.state != CMDLINE_HIDDEN)
		cmdline_redraw();
}

int
cmdline_active(void)
{
	return cl.state != CMDLINE_HIDDEN;
}
