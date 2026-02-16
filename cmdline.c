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
	int cmd_mode; /* 0 = insert, 1 = normal */
	char input[CMDLINE_MAX_INPUT];
	int input_len;
	int cursor;   /* byte offset of cursor within input */
	char errmsg[512];
	/* Command history */
	char history[CMDLINE_HIST_MAX][CMDLINE_MAX_INPUT];
	int hist_count;
	int hist_pos;  /* -1 = live input, 0..count-1 = history index */
	char saved_input[CMDLINE_MAX_INPUT]; /* saved live input while browsing */
	int saved_len;
	int saved_cursor;
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

		/* Compute cursor pixel position */
		cursor_x = tx;
		if (cl.cursor > 0) {
			XftTextExtentsUtf8(xw.dpy, cl.font,
			                   (const FcChar8 *)cl.input,
			                   cl.cursor, &extents);
			cursor_x += extents.xOff;
		}

		if (cl.cmd_mode == 0) {
			/* Insert mode: thin bar cursor */
			XftDrawRect(cl.draw, &cl.curcolor,
			            cursor_x, cmdline_border_top,
			            2, cl.font->ascent + cl.font->descent);
		} else {
			/* Normal mode: block cursor with inverted char */
			XftDrawRect(cl.draw, &cl.curcolor,
			            cursor_x, cmdline_border_top,
			            win.cw, cl.font->ascent + cl.font->descent);
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
			cl.cmd_mode = 0;
			cl.input[0] = '\0';
			cl.input_len = 0;
			cl.cursor = 0;
			cl.hist_pos = -1;
			cmdline_redraw();
		}
		return;
	}

	cl.state = CMDLINE_INPUT;
	cl.cmd_mode = 0;
	cl.input[0] = '\0';
	cl.input_len = 0;
	cl.cursor = 0;
	cl.hist_pos = -1;

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
cmdline_hist_save(const char *cmd)
{
	if (cl.hist_count < CMDLINE_HIST_MAX) {
		strncpy(cl.history[cl.hist_count], cmd, CMDLINE_MAX_INPUT - 1);
		cl.history[cl.hist_count][CMDLINE_MAX_INPUT - 1] = '\0';
		cl.hist_count++;
	} else {
		memmove(cl.history[0], cl.history[1],
		        (CMDLINE_HIST_MAX - 1) * CMDLINE_MAX_INPUT);
		strncpy(cl.history[CMDLINE_HIST_MAX - 1], cmd,
		        CMDLINE_MAX_INPUT - 1);
		cl.history[CMDLINE_HIST_MAX - 1][CMDLINE_MAX_INPUT - 1] = '\0';
	}
	if (debug_mode)
		fprintf(stderr, "cmdline: history saved '%s' (count=%d)\n",
		        cmd, cl.hist_count);
}

static void
cmdline_hist_load(int pos)
{
	if (pos < 0) {
		/* Restore saved live input */
		memcpy(cl.input, cl.saved_input, CMDLINE_MAX_INPUT);
		cl.input_len = cl.saved_len;
		cl.cursor = cl.saved_cursor;
	} else if (pos < cl.hist_count) {
		strcpy(cl.input, cl.history[pos]);
		cl.input_len = strlen(cl.input);
		cl.cursor = cl.input_len;
		/* Clamp cursor for normal mode */
		if (cl.cmd_mode && cl.cursor > 0)
			cl.cursor = cl.input_len - 1;
	}
	cl.hist_pos = pos;
}

static void
cmdline_execute(void)
{
	if (cl.input_len == 0) {
		cmdline_close();
		return;
	}

	/* Save to history before executing */
	cmdline_hist_save(cl.input);

	if (debug_mode)
		fprintf(stderr, "cmdline: execute '%s'\n", cl.input);

	/* No commands implemented yet */
	snprintf(cl.errmsg, sizeof(cl.errmsg),
	         "Not a terminal command: '%s'", cl.input);
	cl.state = CMDLINE_ERROR;
	cmdline_redraw();
}

/* UTF-8 cursor helpers */
static void
cursor_left(void)
{
	if (cl.cursor > 0) {
		cl.cursor--;
		while (cl.cursor > 0 &&
		       (cl.input[cl.cursor] & 0xC0) == 0x80)
			cl.cursor--;
	}
}

static void
cursor_right(void)
{
	if (cl.cursor < cl.input_len) {
		cl.cursor++;
		while (cl.cursor < cl.input_len &&
		       (cl.input[cl.cursor] & 0xC0) == 0x80)
			cl.cursor++;
	}
}

static void
delete_at_cursor(void)
{
	if (cl.cursor < cl.input_len) {
		int next = cl.cursor + 1;
		while (next < cl.input_len &&
		       (cl.input[next] & 0xC0) == 0x80)
			next++;
		memmove(cl.input + cl.cursor, cl.input + next,
		        cl.input_len - next);
		cl.input_len -= (next - cl.cursor);
		cl.input[cl.input_len] = '\0';
	}
}

static void
cmdline_clamp_normal_cursor(void)
{
	/* In normal mode cursor sits on a char, not past end */
	if (cl.cmd_mode && cl.input_len > 0 && cl.cursor >= cl.input_len) {
		cl.cursor = cl.input_len - 1;
		while (cl.cursor > 0 &&
		       (cl.input[cl.cursor] & 0xC0) == 0x80)
			cl.cursor--;
	}
}

static int
cmdline_handle_insert(unsigned long ksym, unsigned int state,
                      const char *buf, int len)
{
	(void)state;

	switch (ksym) {
	case XK_Escape:
		cl.cmd_mode = 1;
		/* Move cursor back one like vim */
		if (cl.cursor > 0)
			cursor_left();
		cmdline_clamp_normal_cursor();
		if (debug_mode)
			fprintf(stderr, "cmdline: insert -> normal\n");
		cmdline_redraw();
		return 1;
	case XK_Return:
	case XK_KP_Enter:
		cmdline_execute();
		return 1;
	case XK_Left:
		cursor_left();
		cmdline_redraw();
		return 1;
	case XK_Right:
		cursor_right();
		cmdline_redraw();
		return 1;
	case XK_Up:
		/* History: older */
		if (cl.hist_count > 0) {
			if (cl.hist_pos == -1) {
				memcpy(cl.saved_input, cl.input, CMDLINE_MAX_INPUT);
				cl.saved_len = cl.input_len;
				cl.saved_cursor = cl.cursor;
				cmdline_hist_load(cl.hist_count - 1);
			} else if (cl.hist_pos > 0) {
				cmdline_hist_load(cl.hist_pos - 1);
			}
			cl.cursor = cl.input_len; /* insert mode: cursor at end */
			cmdline_redraw();
		}
		return 1;
	case XK_Down:
		/* History: newer */
		if (cl.hist_pos >= 0) {
			if (cl.hist_pos < cl.hist_count - 1)
				cmdline_hist_load(cl.hist_pos + 1);
			else
				cmdline_hist_load(-1);
			cl.cursor = cl.input_len;
			cmdline_redraw();
		}
		return 1;
	case XK_Home:
		cl.cursor = 0;
		cmdline_redraw();
		return 1;
	case XK_End:
		cl.cursor = cl.input_len;
		cmdline_redraw();
		return 1;
	case XK_Delete:
		delete_at_cursor();
		cmdline_redraw();
		return 1;
	case XK_BackSpace:
		if (cl.cursor > 0) {
			int prev = cl.cursor - 1;
			while (prev > 0 && (cl.input[prev] & 0xC0) == 0x80)
				prev--;
			memmove(cl.input + prev, cl.input + cl.cursor,
			        cl.input_len - cl.cursor);
			cl.input_len -= (cl.cursor - prev);
			cl.cursor = prev;
			cl.input[cl.input_len] = '\0';
			cmdline_redraw();
		} else if (cl.input_len == 0) {
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
		return 1;
	}
}

static int
cmdline_handle_normal(unsigned long ksym, unsigned int state,
                      const char *buf, int len)
{
	(void)state;
	(void)buf;
	(void)len;

	switch (ksym) {
	case XK_Return:
	case XK_KP_Enter:
		cmdline_execute();
		return 1;
	case 'h':
	case XK_Left:
		cursor_left();
		cmdline_redraw();
		return 1;
	case 'l':
	case XK_Right:
		if (cl.cursor < cl.input_len) {
			cursor_right();
			cmdline_clamp_normal_cursor();
			cmdline_redraw();
		}
		return 1;
	case 'k':
	case XK_Up:
		/* History: older */
		if (cl.hist_count > 0) {
			if (cl.hist_pos == -1) {
				memcpy(cl.saved_input, cl.input, CMDLINE_MAX_INPUT);
				cl.saved_len = cl.input_len;
				cl.saved_cursor = cl.cursor;
				cmdline_hist_load(cl.hist_count - 1);
			} else if (cl.hist_pos > 0) {
				cmdline_hist_load(cl.hist_pos - 1);
			}
			cmdline_clamp_normal_cursor();
			if (debug_mode)
				fprintf(stderr, "cmdline: history[%d]='%s'\n",
				        cl.hist_pos, cl.input);
			cmdline_redraw();
		}
		return 1;
	case 'j':
	case XK_Down:
		/* History: newer */
		if (cl.hist_pos >= 0) {
			if (cl.hist_pos < cl.hist_count - 1)
				cmdline_hist_load(cl.hist_pos + 1);
			else
				cmdline_hist_load(-1);
			cmdline_clamp_normal_cursor();
			if (debug_mode)
				fprintf(stderr, "cmdline: history[%d]='%s'\n",
				        cl.hist_pos, cl.input);
			cmdline_redraw();
		}
		return 1;
	case '0':
	case XK_Home:
		cl.cursor = 0;
		cmdline_redraw();
		return 1;
	case '$':
	case XK_End:
		if (cl.input_len > 0)
			cl.cursor = cl.input_len - 1;
		cmdline_clamp_normal_cursor();
		cmdline_redraw();
		return 1;
	case 'x':
	case XK_Delete:
		delete_at_cursor();
		cmdline_clamp_normal_cursor();
		cmdline_redraw();
		return 1;
	case 'i':
		cl.cmd_mode = 0;
		if (debug_mode)
			fprintf(stderr, "cmdline: normal -> insert\n");
		cmdline_redraw();
		return 1;
	case 'a':
		cl.cmd_mode = 0;
		if (cl.cursor < cl.input_len)
			cursor_right();
		if (debug_mode)
			fprintf(stderr, "cmdline: normal -> insert (append)\n");
		cmdline_redraw();
		return 1;
	case 'I':
		cl.cmd_mode = 0;
		cl.cursor = 0;
		if (debug_mode)
			fprintf(stderr, "cmdline: normal -> insert (bol)\n");
		cmdline_redraw();
		return 1;
	case 'A':
		cl.cmd_mode = 0;
		cl.cursor = cl.input_len;
		if (debug_mode)
			fprintf(stderr, "cmdline: normal -> insert (eol)\n");
		cmdline_redraw();
		return 1;
	default:
		break;
	}

	return 1; /* Consume all keys when cmdline active */
}

int
cmdline_handle_key(unsigned long ksym, unsigned int state,
                   const char *buf, int len)
{
	/* Shift+Escape: close from any state */
	if (ksym == XK_Escape && (state & ShiftMask)) {
		cmdline_close();
		return 1;
	}

	/* In error state: any key dismisses */
	if (cl.state == CMDLINE_ERROR) {
		if (debug_mode)
			fprintf(stderr, "cmdline: error dismissed by keypress\n");
		cmdline_close();
		return 1;
	}

	if (cl.state != CMDLINE_INPUT)
		return 0;

	if (cl.cmd_mode == 0)
		return cmdline_handle_insert(ksym, state, buf, len);
	else
		return cmdline_handle_normal(ksym, state, buf, len);
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
