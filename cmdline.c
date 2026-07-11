/* See LICENSE for license details. */
/* Command-line mode for st terminal (vim-like : command) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef ST_NATIVE_MACOS
#include "macos/keysyms.h"
#include "macos/native.h"
#else
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>
#endif

#include "cmdline.h"
#include "cmdline_layout.h"
#include "notif.h"
#include "vimnav.h"

#ifndef ST_NATIVE_MACOS
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
#endif
extern int debug_mode;
extern int tisaltscreen(void);
extern void xsetsel(char *str);
extern void xclipcopy(void);
extern int xdrawrowtop(int);
extern int xdrawrowbottom(int);

/* Minimal Term redeclaration for term.scr access (same pattern as vimnav.c) */
typedef struct {
	Glyph attr; int x; int y; char state;
} TCursor_cl;
typedef struct {
	int row, col, maxcol;
	Line *line;
	Line *alt;
	Line hist[(1 << 15)];
	int histi;
	int scr;
	int *dirty;
	TCursor_cl c;
	int ocx, ocy, top, bot, mode, esc;
	char trantbl[4];
	int charset, icharset;
	int *tabs;
	Rune lastc;
	int histn;
} Term_cl;
extern Term_cl term;

/* Command-line states */
enum {
	CMDLINE_HIDDEN = 0,
	CMDLINE_INPUT,
	CMDLINE_ERROR,
};

static struct {
#ifndef ST_NATIVE_MACOS
	Window win;
	Drawable buf;
	XftDraw *draw;
	GC gc;
	XftFont *font;
	XftColor fg, bg, err, curcolor, border, sel;
#endif
	int state;
	int cmd_mode; /* 0 = insert, 1 = normal, 2 = visual */
	int anchor;   /* visual mode anchor (byte offset) */
	int pending_op;      /* operator-pending: 'd', 'c', 'y', or 0 */
	int pending_textobj; /* text object prefix: 'i', 'a', or 0 */
	int pending_f;       /* 'f' or 'F' when waiting for seek char */
	char last_f_char[8]; /* last f/F target bytes */
	int last_f_len;      /* byte length of last f/F target */
	int last_f_dir;      /* 1 = forward (f), -1 = backward (F) */
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
	int row_height;
	int content_y;
	int content_height;
	int baseline;
	int y;        /* y position in parent */
	int loaded;
	char prefix;  /* ':' for command, '/' for forward search, '?' for backward */
	/* Saved vimnav position for search (restore on cancel) */
	int search_vim_x, search_vim_y, search_scr;
} cl;

#ifndef ST_NATIVE_MACOS
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
#endif

static int
cmdline_load_resources(void)
{
	if (cl.loaded)
		return 1;

#ifdef ST_NATIVE_MACOS
	cl.loaded = 1;
	return 1;
#else
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
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, cmdline_sel_color, &cl.sel);

	cl.loaded = 1;
	return 1;
#endif
}

static void
cmdline_compute_geometry(void)
{
	int row = term.row > 0 ? term.row - 1 : 0;
#ifdef ST_NATIVE_MACOS
	int ascent = (int)ceil(mac_renderer_ascent());
	int descent = (int)ceil(mac_renderer_descent());
#else
	int ascent = cl.font ? cl.font->ascent : win.ch;
	int descent = cl.font ? cl.font->descent : 0;
#endif
	CmdlineLayout l = cmdline_layout(win.w, win.h,
	                                  xdrawrowtop(row),
	                                  xdrawrowbottom(row),
	                                  ascent, descent,
	                                  cmdline_border_top);

	cl.y = l.y;
	cl.width = l.width;
	cl.height = l.height;
	cl.row_height = l.row_height;
	cl.content_y = l.content_y;
	cl.content_height = l.content_height;
	cl.baseline = l.baseline;
}

void
cmdline_init(void)
{
#ifndef ST_NATIVE_MACOS
	XSetWindowAttributes attrs;
	XGCValues gcvalues;
#endif

	if (!cmdline_load_resources())
		return;

	cmdline_compute_geometry();

#ifndef ST_NATIVE_MACOS
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
#endif

	cl.state = CMDLINE_HIDDEN;

	if (debug_mode)
		fprintf(stderr, "cmdline: initialized (y=%d, %dx%d)\n",
		        cl.y, cl.width, cl.height);
}

static void
cmdline_redraw(void)
{
#ifdef ST_NATIVE_MACOS
	macos_request_redraw();
#else
	int tx, ty;
	int cursor_x;
	XGlyphInfo extents;

	if (!cl.draw || !cl.font)
		return;

	/* Clear background */
	XftDrawRect(cl.draw, &cl.bg, 0, 0, cl.width, cl.height);

	/* Draw top border line */
	XftDrawRect(cl.draw, &cl.border, 0, 0, cl.width, cmdline_border_top);

	ty = cl.baseline;
	tx = win.cw / 2;

	if (cl.state == CMDLINE_INPUT) {
		int cy = cl.content_y;
		int cheight = cl.content_height;

		/* Draw prefix character (:, /, or ?) */
		XftDrawStringUtf8(cl.draw, &cl.fg, cl.font, tx, ty,
		                  (const FcChar8 *)&cl.prefix, 1);
		tx += win.cw;

		/* Visual mode: draw selection highlight behind text */
		if (cl.cmd_mode == 2 && cl.input_len > 0) {
			int sel_start = cl.anchor < cl.cursor ? cl.anchor : cl.cursor;
			int sel_end = cl.anchor < cl.cursor ? cl.cursor : cl.anchor;
			int sx, ex;

			/* Find end of character at sel_end for inclusive selection */
			int sel_end_next = sel_end + 1;
			while (sel_end_next < cl.input_len &&
			       (cl.input[sel_end_next] & 0xC0) == 0x80)
				sel_end_next++;

			sx = tx;
			if (sel_start > 0) {
				XftTextExtentsUtf8(xw.dpy, cl.font,
				                   (const FcChar8 *)cl.input,
				                   sel_start, &extents);
				sx += extents.xOff;
			}
			XftTextExtentsUtf8(xw.dpy, cl.font,
			                   (const FcChar8 *)cl.input,
			                   sel_end_next, &extents);
			ex = tx + extents.xOff;

			XftDrawRect(cl.draw, &cl.sel,
			            sx, cy,
			            ex - sx, cheight);
		}

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
			            cursor_x, cy,
			            2, cheight);
		} else {
			/* Normal/visual mode: block cursor with inverted char */
			XftDrawRect(cl.draw, &cl.curcolor,
			            cursor_x, cy,
			            win.cw, cheight);
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
#endif
}

static void
cmdline_open_with_prefix(char prefix)
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
			cl.pending_op = 0;
			cl.pending_textobj = 0;
			cl.pending_f = 0;
			cl.prefix = prefix;
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
	cl.pending_op = 0;
	cl.pending_textobj = 0;
	cl.pending_f = 0;
	cl.prefix = prefix;

	/* GPU initialization can happen after cmdline_init(), and the GPU renderer may
	 * use scaled row geometry when the window size is not an exact cell multiple.
	 * Recompute just before mapping so the overlay tracks the actual rendered
	 * bottom row instead of the startup/default geometry. */
	cmdline_resize();
#ifndef ST_NATIVE_MACOS
	XMapRaised(xw.dpy, cl.win);
#endif
	cmdline_redraw();

	if (debug_mode)
		fprintf(stderr, "cmdline: opened prefix='%c'\n", prefix);
}

void
cmdline_open(void)
{
	cmdline_open_with_prefix(':');
}

void
cmdline_open_search(int forward)
{
	cl.search_vim_x = vimnav.x;
	cl.search_vim_y = vimnav.y;
	cl.search_scr = term.scr;
	cmdline_open_with_prefix(forward ? '/' : '?');
}

static void
cmdline_search_cancel(void)
{
	search_clear();
	vimnav.ox = vimnav.x;
	vimnav.oy = vimnav.y;
	vimnav.x = cl.search_vim_x;
	vimnav.y = cl.search_vim_y;
	term.scr = cl.search_scr;
	tfulldirt();
}

static void
cmdline_live_search(void)
{
	if (cl.prefix != '/' && cl.prefix != '?')
		return;
	if (cl.input_len == 0) {
		if (search_active())
			search_clear();
		/* Restore to original position */
		vimnav.ox = vimnav.x;
		vimnav.oy = vimnav.y;
		vimnav.x = cl.search_vim_x;
		vimnav.y = cl.search_vim_y;
		term.scr = cl.search_scr;
		tfulldirt();
		return;
	}
	/* Reset to original position before searching */
	vimnav.x = cl.search_vim_x;
	vimnav.y = cl.search_vim_y;
	term.scr = cl.search_scr;
	search_execute(cl.input, cl.prefix == '/' ? 1 : -1);
}

void
cmdline_close(void)
{
	if (cl.state == CMDLINE_HIDDEN)
		return;

	cl.state = CMDLINE_HIDDEN;
	cl.pending_op = 0;
	cl.pending_textobj = 0;
	cl.pending_f = 0;
#ifndef ST_NATIVE_MACOS
	XUnmapWindow(xw.dpy, cl.win);
#endif
	cmdline_redraw();

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

/* Parse and execute :notify command.
 * Syntax: notify [-t ms] [-bg col] [-fg col] [-b col] [-ts sz] <msg>
 * <msg> is unquoted (single word) or "quoted with spaces" (\" for inner quotes).
 * Unquoted message takes rest of line. Returns 1 on success, 0 on error. */
static int
cmdline_exec_notify(const char *args)
{
	char meta[512];
	char msg[512];
	char wire[1024];
	int mlen = 0;
	const char *p = args;

	/* Skip leading whitespace */
	while (*p == ' ' || *p == '\t')
		p++;

	if (*p == '\0') {
		snprintf(cl.errmsg, sizeof(cl.errmsg), "notify: missing message");
		return 0;
	}

	/* Parse options */
	while (*p == '-') {
		const char *start = p;
		char key[8];
		char val[64];
		int vi = 0;

		p++; /* skip '-' */

		/* Match known flag names (multi-char before single-char) */
		if (strncmp(p, "bg", 2) == 0 && (p[2] == ' ' || p[2] == '\t')) {
			strcpy(key, "bg");
			p += 2;
		} else if (strncmp(p, "fg", 2) == 0 && (p[2] == ' ' || p[2] == '\t')) {
			strcpy(key, "fg");
			p += 2;
		} else if (strncmp(p, "ts", 2) == 0 && (p[2] == ' ' || p[2] == '\t')) {
			strcpy(key, "ts");
			p += 2;
		} else if (*p == 't' && (p[1] == ' ' || p[1] == '\t')) {
			strcpy(key, "t");
			p += 1;
		} else if (*p == 'b' && (p[1] == ' ' || p[1] == '\t')) {
			strcpy(key, "b");
			p += 1;
		} else {
			/* Not a recognized flag — rest is the message */
			p = start;
			break;
		}

		/* Skip whitespace before value */
		while (*p == ' ' || *p == '\t')
			p++;

		/* Parse value (quoted or unquoted) */
		if (*p == '"') {
			p++;
			while (*p && *p != '"' && vi < (int)sizeof(val) - 1) {
				if (*p == '\\' && p[1] == '"') {
					val[vi++] = '"';
					p += 2;
				} else {
					val[vi++] = *p++;
				}
			}
			if (*p == '"')
				p++;
		} else {
			while (*p && *p != ' ' && *p != '\t' && vi < (int)sizeof(val) - 1)
				val[vi++] = *p++;
		}
		val[vi] = '\0';

		if (vi == 0) {
			snprintf(cl.errmsg, sizeof(cl.errmsg),
			         "notify: missing value for -%s", key);
			return 0;
		}

		/* Append to metadata (wire protocol: key=val\x1f separated) */
		if (mlen > 0)
			meta[mlen++] = '\x1f';
		mlen += snprintf(meta + mlen, sizeof(meta) - mlen,
		                 "%s=%s", key, val);

		/* Skip whitespace before next option or message */
		while (*p == ' ' || *p == '\t')
			p++;
	}

	/* Parse message */
	if (*p == '\0') {
		snprintf(cl.errmsg, sizeof(cl.errmsg), "notify: missing message");
		return 0;
	}

	if (*p == '"') {
		int mi = 0;
		p++;
		while (*p && mi < (int)sizeof(msg) - 1) {
			if (*p == '\\' && p[1] == '"') {
				msg[mi++] = '"';
				p += 2;
			} else if (*p == '"') {
				break;
			} else {
				msg[mi++] = *p++;
			}
		}
		msg[mi] = '\0';
	} else {
		/* Unquoted: rest of input is the message */
		strncpy(msg, p, sizeof(msg) - 1);
		msg[sizeof(msg) - 1] = '\0';
	}

	if (msg[0] == '\0') {
		snprintf(cl.errmsg, sizeof(cl.errmsg), "notify: empty message");
		return 0;
	}

	/* Build wire protocol string: metadata\x1emessage (or just message) */
	if (mlen > 0)
		snprintf(wire, sizeof(wire), "%s\x1e%s", meta, msg);
	else
		snprintf(wire, sizeof(wire), "%s", msg);

	notif_show(wire);
	return 1;
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
		fprintf(stderr, "cmdline: execute prefix='%c' input='%s'\n",
		        cl.prefix, cl.input);

	/* Search mode: / or ? — search already running live, just confirm and close */
	if (cl.prefix == '/' || cl.prefix == '?') {
		cmdline_close();
		return;
	}

	/* Command mode: : */
	if (strcmp(cl.input, "noh") == 0) {
		search_noh();
		cmdline_close();
		return;
	}

	/* :notify command */
	if (strncmp(cl.input, "notify", 6) == 0 &&
	    (cl.input[6] == ' ' || cl.input[6] == '\0')) {
		if (cmdline_exec_notify(cl.input + 6)) {
			cmdline_close();
		} else {
			cl.state = CMDLINE_ERROR;
			cmdline_redraw();
		}
		return;
	}

	/* Unknown command */
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
	/* In normal/visual mode cursor sits on a char, not past end */
	if (cl.cmd_mode >= 1 && cl.input_len > 0 && cl.cursor >= cl.input_len) {
		cl.cursor = cl.input_len - 1;
		while (cl.cursor > 0 &&
		       (cl.input[cl.cursor] & 0xC0) == 0x80)
			cl.cursor--;
	}
}

/* ---- Text object helpers ---- */

static int
is_word_char(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_';
}

static int
cmdline_textobj_word(int inner, int *s, int *e)
{
	int pos = cl.cursor;
	int start, end;
	unsigned char ch;

	if (pos >= cl.input_len)
		return 0;

	ch = (unsigned char)cl.input[pos];
	if (is_word_char(ch)) {
		start = pos;
		while (start > 0 && is_word_char((unsigned char)cl.input[start - 1]))
			start--;
		end = pos;
		while (end < cl.input_len && is_word_char((unsigned char)cl.input[end]))
			end++;
	} else if (ch == ' ' || ch == '\t') {
		start = pos;
		while (start > 0 && (cl.input[start - 1] == ' ' || cl.input[start - 1] == '\t'))
			start--;
		end = pos;
		while (end < cl.input_len && (cl.input[end] == ' ' || cl.input[end] == '\t'))
			end++;
	} else {
		start = pos;
		while (start > 0 && !is_word_char((unsigned char)cl.input[start - 1]) &&
		       cl.input[start - 1] != ' ' && cl.input[start - 1] != '\t')
			start--;
		end = pos;
		while (end < cl.input_len && !is_word_char((unsigned char)cl.input[end]) &&
		       cl.input[end] != ' ' && cl.input[end] != '\t')
			end++;
	}

	if (!inner) {
		if (end < cl.input_len && (cl.input[end] == ' ' || cl.input[end] == '\t')) {
			while (end < cl.input_len && (cl.input[end] == ' ' || cl.input[end] == '\t'))
				end++;
		} else {
			while (start > 0 && (cl.input[start - 1] == ' ' || cl.input[start - 1] == '\t'))
				start--;
		}
	}

	*s = start;
	*e = end;
	return 1;
}

static int
cmdline_textobj_WORD(int inner, int *s, int *e)
{
	int pos = cl.cursor;
	int start, end;

	if (pos >= cl.input_len)
		return 0;

	if (cl.input[pos] == ' ' || cl.input[pos] == '\t') {
		start = pos;
		while (start > 0 && (cl.input[start - 1] == ' ' || cl.input[start - 1] == '\t'))
			start--;
		end = pos;
		while (end < cl.input_len && (cl.input[end] == ' ' || cl.input[end] == '\t'))
			end++;
	} else {
		start = pos;
		while (start > 0 && cl.input[start - 1] != ' ' && cl.input[start - 1] != '\t')
			start--;
		end = pos;
		while (end < cl.input_len && cl.input[end] != ' ' && cl.input[end] != '\t')
			end++;
	}

	if (!inner) {
		if (end < cl.input_len && (cl.input[end] == ' ' || cl.input[end] == '\t')) {
			while (end < cl.input_len && (cl.input[end] == ' ' || cl.input[end] == '\t'))
				end++;
		} else {
			while (start > 0 && (cl.input[start - 1] == ' ' || cl.input[start - 1] == '\t'))
				start--;
		}
	}

	*s = start;
	*e = end;
	return 1;
}

static int
cmdline_textobj_quote(int inner, char q, int *s, int *e)
{
	int i, left = -1, right = -1;

	/* Try to find enclosing pair around cursor */
	for (i = cl.cursor; i >= 0; i--) {
		if (cl.input[i] == q) {
			int j, jstart = (i == cl.cursor) ? cl.cursor + 1 : cl.cursor;
			for (j = jstart; j < cl.input_len; j++) {
				if (cl.input[j] == q) {
					left = i;
					right = j;
					goto found;
				}
			}
		}
	}

	/* Forward search: find first pair ahead of cursor */
	for (i = cl.cursor; i < cl.input_len; i++) {
		if (cl.input[i] == q) {
			int j;
			for (j = i + 1; j < cl.input_len; j++) {
				if (cl.input[j] == q) {
					left = i;
					right = j;
					goto found;
				}
			}
			break; /* Only one quote ahead, no pair */
		}
	}
	return 0;

found:
	if (inner) {
		*s = left + 1;
		*e = right;
	} else {
		*s = left;
		*e = right + 1;
	}
	return 1;
}

static int
cmdline_textobj_bracket(int inner, char open, char close, int *s, int *e)
{
	int i, depth, left = -1, right = -1;

	/* Try to find enclosing pair around cursor */
	depth = 0;
	for (i = cl.cursor; i >= 0; i--) {
		if (cl.input[i] == close && i < cl.cursor)
			depth++;
		if (cl.input[i] == open) {
			if (depth == 0) { left = i; break; }
			depth--;
		}
	}

	if (left >= 0) {
		/* Found opening to the left, find matching close */
		depth = 0;
		for (i = left + 1; i < cl.input_len; i++) {
			if (cl.input[i] == open)
				depth++;
			if (cl.input[i] == close) {
				if (depth == 0) { right = i; break; }
				depth--;
			}
		}
		if (right >= 0)
			goto found;
	}

	/* Forward search: find first opening bracket ahead of cursor */
	left = -1;
	right = -1;
	for (i = cl.cursor; i < cl.input_len; i++) {
		if (cl.input[i] == open) {
			left = i;
			break;
		}
	}
	if (left < 0)
		return 0;

	depth = 0;
	for (i = left + 1; i < cl.input_len; i++) {
		if (cl.input[i] == open)
			depth++;
		if (cl.input[i] == close) {
			if (depth == 0) { right = i; break; }
			depth--;
		}
	}
	if (right < 0)
		return 0;

found:
	if (inner) {
		*s = left + 1;
		*e = right;
	} else {
		*s = left;
		*e = right + 1;
	}
	return 1;
}

/* Unified text object dispatcher. Returns byte range [s, e) on success. */
static int
cmdline_find_textobj(int ia, unsigned long key, int *s, int *e)
{
	int inner = (ia == 'i');

	switch (key) {
	case 'w':
		return cmdline_textobj_word(inner, s, e);
	case 'W':
		return cmdline_textobj_WORD(inner, s, e);
	case '"':
		return cmdline_textobj_quote(inner, '"', s, e);
	case '\'':
		return cmdline_textobj_quote(inner, '\'', s, e);
	case '`':
		return cmdline_textobj_quote(inner, '`', s, e);
	case '(':
	case ')':
	case 'b':
		return cmdline_textobj_bracket(inner, '(', ')', s, e);
	case '{':
	case '}':
	case 'B':
		return cmdline_textobj_bracket(inner, '{', '}', s, e);
	case '[':
	case ']':
		return cmdline_textobj_bracket(inner, '[', ']', s, e);
	case '<':
	case '>':
		return cmdline_textobj_bracket(inner, '<', '>', s, e);
	default:
		return 0;
	}
}

/* ---- Word motion helpers ---- */

/* Character category: 0=whitespace, 1=word, 2=other */
static int
char_cat(unsigned char c)
{
	if (c == ' ' || c == '\t')
		return 0;
	if (is_word_char(c))
		return 1;
	return 2;
}

/* WORD category: 0=whitespace, 1=non-whitespace */
static int
char_CAT(unsigned char c)
{
	return (c != ' ' && c != '\t') ? 1 : 0;
}

/* w motion: forward to start of next word. Returns new byte offset. */
static int
motion_w_pos(int pos)
{
	int cat;

	if (pos >= cl.input_len)
		return pos;
	cat = char_cat((unsigned char)cl.input[pos]);
	while (pos < cl.input_len &&
	       char_cat((unsigned char)cl.input[pos]) == cat)
		pos++;
	while (pos < cl.input_len &&
	       char_cat((unsigned char)cl.input[pos]) == 0)
		pos++;
	return pos;
}

/* W motion: forward to start of next WORD. */
static int
motion_W_pos(int pos)
{
	int cat;

	if (pos >= cl.input_len)
		return pos;
	cat = char_CAT((unsigned char)cl.input[pos]);
	while (pos < cl.input_len &&
	       char_CAT((unsigned char)cl.input[pos]) == cat)
		pos++;
	while (pos < cl.input_len &&
	       char_CAT((unsigned char)cl.input[pos]) == 0)
		pos++;
	return pos;
}

/* e motion: forward to end of word. */
static int
motion_e_pos(int pos)
{
	int cat, orig = pos;

	if (pos >= cl.input_len - 1)
		return pos;
	pos++;
	while (pos < cl.input_len &&
	       char_cat((unsigned char)cl.input[pos]) == 0)
		pos++;
	if (pos >= cl.input_len)
		return orig;
	cat = char_cat((unsigned char)cl.input[pos]);
	while (pos + 1 < cl.input_len &&
	       char_cat((unsigned char)cl.input[pos + 1]) == cat)
		pos++;
	/* Back up to start of UTF-8 character */
	while (pos > 0 && (cl.input[pos] & 0xC0) == 0x80)
		pos--;
	return pos;
}

/* E motion: forward to end of WORD. */
static int
motion_E_pos(int pos)
{
	int cat, orig = pos;

	if (pos >= cl.input_len - 1)
		return pos;
	pos++;
	while (pos < cl.input_len &&
	       char_CAT((unsigned char)cl.input[pos]) == 0)
		pos++;
	if (pos >= cl.input_len)
		return orig;
	cat = char_CAT((unsigned char)cl.input[pos]);
	while (pos + 1 < cl.input_len &&
	       char_CAT((unsigned char)cl.input[pos + 1]) == cat)
		pos++;
	/* Back up to start of UTF-8 character */
	while (pos > 0 && (cl.input[pos] & 0xC0) == 0x80)
		pos--;
	return pos;
}

/* b motion: backward to start of word. */
static int
motion_b_pos(int pos)
{
	int cat;

	if (pos <= 0)
		return 0;
	pos--;
	while (pos > 0 &&
	       char_cat((unsigned char)cl.input[pos]) == 0)
		pos--;
	cat = char_cat((unsigned char)cl.input[pos]);
	while (pos > 0 &&
	       char_cat((unsigned char)cl.input[pos - 1]) == cat)
		pos--;
	return pos;
}

/* B motion: backward to start of WORD. */
static int
motion_B_pos(int pos)
{
	int cat;

	if (pos <= 0)
		return 0;
	pos--;
	while (pos > 0 &&
	       char_CAT((unsigned char)cl.input[pos]) == 0)
		pos--;
	cat = char_CAT((unsigned char)cl.input[pos]);
	while (pos > 0 &&
	       char_CAT((unsigned char)cl.input[pos - 1]) == cat)
		pos--;
	return pos;
}

/* Forward declaration for cmdline_do_seek */
static void cmdline_exec_op_range(int op, int s, int e);

/* ---- Character seek helpers (f/F) ---- */

/* Seek forward from cursor for character ch[0..chlen-1].
 * Returns byte offset of match, or -1 if not found. */
static int
cmdline_seek_forward(const char *ch, int chlen)
{
	int pos = cl.cursor;

	/* Advance past current character */
	pos++;
	while (pos < cl.input_len && (cl.input[pos] & 0xC0) == 0x80)
		pos++;
	/* Scan forward for match */
	while (pos + chlen <= cl.input_len) {
		if (memcmp(cl.input + pos, ch, chlen) == 0)
			return pos;
		pos++;
		while (pos < cl.input_len && (cl.input[pos] & 0xC0) == 0x80)
			pos++;
	}
	return -1;
}

/* Seek backward from cursor for character ch[0..chlen-1].
 * Returns byte offset of match, or -1 if not found. */
static int
cmdline_seek_backward(const char *ch, int chlen)
{
	int pos = cl.cursor;

	if (pos <= 0)
		return -1;
	pos--;
	while (pos > 0 && (cl.input[pos] & 0xC0) == 0x80)
		pos--;
	while (pos >= 0) {
		if (pos + chlen <= cl.input_len &&
		    memcmp(cl.input + pos, ch, chlen) == 0)
			return pos;
		if (pos == 0)
			break;
		pos--;
		while (pos > 0 && (cl.input[pos] & 0xC0) == 0x80)
			pos--;
	}
	return -1;
}

/* Execute a character seek. If pending_op is set, operates on the range
 * (f/F are inclusive motions). Otherwise just moves cursor.
 * Returns 1 if a match was found. */
static int
cmdline_do_seek(const char *ch, int chlen, int dir)
{
	int dest = (dir > 0) ? cmdline_seek_forward(ch, chlen)
	                     : cmdline_seek_backward(ch, chlen);
	if (dest < 0)
		return 0;

	if (cl.pending_op) {
		int s, e;
		if (dest > cl.cursor) {
			s = cl.cursor;
			/* inclusive: include char at dest */
			e = dest + 1;
			while (e < cl.input_len &&
			       (cl.input[e] & 0xC0) == 0x80)
				e++;
		} else {
			s = dest;
			/* inclusive: include char at cursor */
			e = cl.cursor + 1;
			while (e < cl.input_len &&
			       (cl.input[e] & 0xC0) == 0x80)
				e++;
		}
		if (e > cl.input_len)
			e = cl.input_len;
		cmdline_exec_op_range(cl.pending_op, s, e);
		cl.pending_op = 0;
	} else {
		cl.cursor = dest;
	}
	return 1;
}

/* Execute operator on a byte range [s, e) */
static void
cmdline_exec_op_range(int op, int s, int e)
{
	if (e <= s)
		return;

	switch (op) {
	case 'y': {
		char *text = xmalloc(e - s + 1);
		memcpy(text, cl.input + s, e - s);
		text[e - s] = '\0';
		xsetsel(text);
		xclipcopy();
		if (debug_mode)
			fprintf(stderr, "cmdline: op yank '%s'\n", text);
		break;
	}
	case 'd':
		memmove(cl.input + s, cl.input + e, cl.input_len - e);
		cl.input_len -= (e - s);
		cl.input[cl.input_len] = '\0';
		cl.cursor = s;
		cmdline_clamp_normal_cursor();
		if (debug_mode)
			fprintf(stderr, "cmdline: op delete, input='%s'\n", cl.input);
		break;
	case 'c':
		memmove(cl.input + s, cl.input + e, cl.input_len - e);
		cl.input_len -= (e - s);
		cl.input[cl.input_len] = '\0';
		cl.cursor = s;
		cl.cmd_mode = 0; /* insert mode */
		if (debug_mode)
			fprintf(stderr, "cmdline: op change, input='%s'\n", cl.input);
		break;
	}
}

static int
cmdline_handle_insert(unsigned long ksym, unsigned int state,
                      const char *buf, int len)
{
	(void)state;

	switch (ksym) {
	case XK_Escape:
		/* In search mode, Escape cancels search (like vim) */
		if (cl.prefix == '/' || cl.prefix == '?') {
			cmdline_search_cancel();
			cmdline_close();
			return 1;
		}
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
			if (cl.prefix == '/' || cl.prefix == '?')
				cmdline_search_cancel();
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

	/* Pending char seek: consume next key as target */
	if (cl.pending_f) {
		if (len > 0 && (unsigned char)buf[0] >= 0x20) {
			int dir = (cl.pending_f == 'f') ? 1 : -1;
			if (cmdline_do_seek(buf, len, dir)) {
				memcpy(cl.last_f_char, buf, len);
				cl.last_f_len = len;
				cl.last_f_dir = dir;
			}
		}
		cl.pending_f = 0;
		cl.pending_op = 0;
		cmdline_redraw();
		return 1;
	}

	/* Pending text object: waiting for object key */
	if (cl.pending_op && cl.pending_textobj) {
		int s, e;
		if (cmdline_find_textobj(cl.pending_textobj, ksym, &s, &e))
			cmdline_exec_op_range(cl.pending_op, s, e);
		cl.pending_op = 0;
		cl.pending_textobj = 0;
		cmdline_redraw();
		return 1;
	}

	/* Pending operator: waiting for i/a or doubled key */
	if (cl.pending_op) {
		if (ksym == 'i' || ksym == 'a') {
			cl.pending_textobj = ksym;
			return 1;
		}
		if ((int)ksym == cl.pending_op) {
			/* dd/cc/yy - operate on whole line */
			cmdline_exec_op_range(cl.pending_op, 0, cl.input_len);
			cl.pending_op = 0;
			cmdline_redraw();
			return 1;
		}
		/* Motion as operator target (dw, cb, ye, etc.) */
		{
			int dest = -1, s, e;
			switch (ksym) {
			case 'w': dest = motion_w_pos(cl.cursor); break;
			case 'W': dest = motion_W_pos(cl.cursor); break;
			case 'e': dest = motion_e_pos(cl.cursor); break;
			case 'E': dest = motion_E_pos(cl.cursor); break;
			case 'b': dest = motion_b_pos(cl.cursor); break;
			case 'B': dest = motion_B_pos(cl.cursor); break;
			}
			if (dest >= 0 && dest != cl.cursor) {
				if (dest > cl.cursor) {
					s = cl.cursor;
					/* e/E are inclusive */
					if (ksym == 'e' || ksym == 'E') {
						e = dest + 1;
						while (e < cl.input_len &&
						       (cl.input[e] & 0xC0) == 0x80)
							e++;
					} else {
						e = dest;
					}
				} else {
					s = dest;
					e = cl.cursor;
				}
				if (e > cl.input_len)
					e = cl.input_len;
				cmdline_exec_op_range(cl.pending_op, s, e);
				cl.pending_op = 0;
				cmdline_redraw();
				return 1;
			}
		}
		/* f/F: set pending seek, keep pending_op */
		if (ksym == 'f' || ksym == 'F') {
			cl.pending_f = ksym;
			return 1;
		}
		/* ;/, as operator targets */
		if (ksym == ';' || ksym == ',') {
			if (cl.last_f_len > 0) {
				int dir = (ksym == ';') ? cl.last_f_dir : -cl.last_f_dir;
				cmdline_do_seek(cl.last_f_char, cl.last_f_len, dir);
			}
			cl.pending_op = 0;
			cmdline_redraw();
			return 1;
		}
		/* Unknown key - cancel pending */
		cl.pending_op = 0;
		return 1;
	}

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
	/* Operator-pending keys */
	case 'd':
		cl.pending_op = 'd';
		return 1;
	case 'c':
		cl.pending_op = 'c';
		return 1;
	case 'y':
		cl.pending_op = 'y';
		return 1;
	/* Shift+D: delete from cursor to end */
	case 'D':
		if (cl.input_len > 0) {
			cl.input_len = cl.cursor;
			cl.input[cl.input_len] = '\0';
			cmdline_clamp_normal_cursor();
			if (debug_mode)
				fprintf(stderr, "cmdline: D, input='%s'\n", cl.input);
			cmdline_redraw();
		}
		return 1;
	/* Shift+C: change from cursor to end */
	case 'C':
		cl.input_len = cl.cursor;
		cl.input[cl.input_len] = '\0';
		cl.cmd_mode = 0;
		if (debug_mode)
			fprintf(stderr, "cmdline: C, input='%s'\n", cl.input);
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
	case 'v':
		if (cl.input_len > 0) {
			cl.cmd_mode = 2;
			cl.anchor = cl.cursor;
			if (debug_mode)
				fprintf(stderr, "cmdline: normal -> visual\n");
			cmdline_redraw();
		}
		return 1;
	/* Word motions */
	case 'w':
		cl.cursor = motion_w_pos(cl.cursor);
		cmdline_clamp_normal_cursor();
		cmdline_redraw();
		return 1;
	case 'W':
		cl.cursor = motion_W_pos(cl.cursor);
		cmdline_clamp_normal_cursor();
		cmdline_redraw();
		return 1;
	case 'e':
		cl.cursor = motion_e_pos(cl.cursor);
		cmdline_clamp_normal_cursor();
		cmdline_redraw();
		return 1;
	case 'E':
		cl.cursor = motion_E_pos(cl.cursor);
		cmdline_clamp_normal_cursor();
		cmdline_redraw();
		return 1;
	case 'b':
		cl.cursor = motion_b_pos(cl.cursor);
		cmdline_redraw();
		return 1;
	case 'B':
		cl.cursor = motion_B_pos(cl.cursor);
		cmdline_redraw();
		return 1;
	/* Character seek */
	case 'f':
		cl.pending_f = 'f';
		return 1;
	case 'F':
		cl.pending_f = 'F';
		return 1;
	case ';':
		if (cl.last_f_len > 0) {
			cmdline_do_seek(cl.last_f_char, cl.last_f_len,
			                cl.last_f_dir);
			cmdline_redraw();
		}
		return 1;
	case ',':
		if (cl.last_f_len > 0) {
			cmdline_do_seek(cl.last_f_char, cl.last_f_len,
			                -cl.last_f_dir);
			cmdline_redraw();
		}
		return 1;
	default:
		break;
	}

	return 1;
}

static void
cmdline_visual_sel_range(int *start, int *end_next)
{
	int s = cl.anchor < cl.cursor ? cl.anchor : cl.cursor;
	int e = cl.anchor < cl.cursor ? cl.cursor : cl.anchor;

	/* end_next: byte past the last selected char (inclusive selection) */
	e++;
	while (e < cl.input_len && (cl.input[e] & 0xC0) == 0x80)
		e++;

	*start = s;
	*end_next = e;
}

static void
cmdline_visual_yank(void)
{
	int start, end_next;
	char *text;

	cmdline_visual_sel_range(&start, &end_next);
	if (end_next <= start)
		return;

	text = xmalloc(end_next - start + 1);
	memcpy(text, cl.input + start, end_next - start);
	text[end_next - start] = '\0';
	xsetsel(text);
	xclipcopy();
	if (debug_mode)
		fprintf(stderr, "cmdline: yanked '%s'\n", text);

	cl.cmd_mode = 1; /* back to normal */
	cmdline_redraw();
}

static void
cmdline_visual_delete(void)
{
	int start, end_next;

	cmdline_visual_sel_range(&start, &end_next);
	if (end_next <= start)
		return;

	memmove(cl.input + start, cl.input + end_next,
	        cl.input_len - end_next);
	cl.input_len -= (end_next - start);
	cl.input[cl.input_len] = '\0';
	cl.cursor = start;

	cl.cmd_mode = 1; /* back to normal */
	cmdline_clamp_normal_cursor();

	if (debug_mode)
		fprintf(stderr, "cmdline: visual delete, input='%s'\n", cl.input);
	cmdline_redraw();
}

static void
cmdline_visual_change(void)
{
	int start, end_next;

	cmdline_visual_sel_range(&start, &end_next);
	if (end_next <= start)
		return;

	memmove(cl.input + start, cl.input + end_next,
	        cl.input_len - end_next);
	cl.input_len -= (end_next - start);
	cl.input[cl.input_len] = '\0';
	cl.cursor = start;

	cl.cmd_mode = 0; /* to insert mode */

	if (debug_mode)
		fprintf(stderr, "cmdline: visual change, input='%s'\n", cl.input);
	cmdline_redraw();
}

static int
cmdline_handle_visual(unsigned long ksym, unsigned int state,
                      const char *buf, int len)
{
	(void)state;

	/* Pending char seek: consume next key as target */
	if (cl.pending_f) {
		if (len > 0 && (unsigned char)buf[0] >= 0x20) {
			int dir = (cl.pending_f == 'f') ? 1 : -1;
			if (cmdline_do_seek(buf, len, dir)) {
				memcpy(cl.last_f_char, buf, len);
				cl.last_f_len = len;
				cl.last_f_dir = dir;
			}
		}
		cl.pending_f = 0;
		cmdline_redraw();
		return 1;
	}

	/* Pending text object: adjust selection */
	if (cl.pending_textobj) {
		int s, e;
		if (cmdline_find_textobj(cl.pending_textobj, ksym, &s, &e) && e > s) {
			cl.anchor = s;
			/* cursor on last char (inclusive) */
			cl.cursor = e - 1;
			while (cl.cursor > s &&
			       (cl.input[cl.cursor] & 0xC0) == 0x80)
				cl.cursor--;
			if (debug_mode)
				fprintf(stderr, "cmdline: visual textobj anchor=%d cursor=%d\n",
				        cl.anchor, cl.cursor);
		}
		cl.pending_textobj = 0;
		cmdline_redraw();
		return 1;
	}

	switch (ksym) {
	case XK_Escape:
	case 'v':
		/* Exit visual mode back to normal */
		cl.cmd_mode = 1;
		if (debug_mode)
			fprintf(stderr, "cmdline: visual -> normal\n");
		cmdline_redraw();
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
	case 'o':
		/* Swap cursor and anchor */
		{
			int tmp = cl.anchor;
			cl.anchor = cl.cursor;
			cl.cursor = tmp;
		}
		if (debug_mode)
			fprintf(stderr, "cmdline: visual swap anchor=%d cursor=%d\n",
			        cl.anchor, cl.cursor);
		cmdline_redraw();
		return 1;
	case 'y':
		cmdline_visual_yank();
		return 1;
	case 'x':
	case 'd':
	case XK_Delete:
		cmdline_visual_delete();
		return 1;
	case 'c':
		cmdline_visual_change();
		return 1;
	/* Word motions */
	case 'w':
		cl.cursor = motion_w_pos(cl.cursor);
		cmdline_clamp_normal_cursor();
		cmdline_redraw();
		return 1;
	case 'W':
		cl.cursor = motion_W_pos(cl.cursor);
		cmdline_clamp_normal_cursor();
		cmdline_redraw();
		return 1;
	case 'e':
		cl.cursor = motion_e_pos(cl.cursor);
		cmdline_clamp_normal_cursor();
		cmdline_redraw();
		return 1;
	case 'E':
		cl.cursor = motion_E_pos(cl.cursor);
		cmdline_clamp_normal_cursor();
		cmdline_redraw();
		return 1;
	case 'b':
		cl.cursor = motion_b_pos(cl.cursor);
		cmdline_redraw();
		return 1;
	case 'B':
		cl.cursor = motion_B_pos(cl.cursor);
		cmdline_redraw();
		return 1;
	/* Character seek */
	case 'f':
		cl.pending_f = 'f';
		return 1;
	case 'F':
		cl.pending_f = 'F';
		return 1;
	case ';':
		if (cl.last_f_len > 0) {
			cmdline_do_seek(cl.last_f_char, cl.last_f_len,
			                cl.last_f_dir);
			cmdline_redraw();
		}
		return 1;
	case ',':
		if (cl.last_f_len > 0) {
			cmdline_do_seek(cl.last_f_char, cl.last_f_len,
			                -cl.last_f_dir);
			cmdline_redraw();
		}
		return 1;
	/* Text object selection */
	case 'i':
		cl.pending_textobj = 'i';
		return 1;
	case 'a':
		cl.pending_textobj = 'a';
		return 1;
	default:
		break;
	}

	return 1; /* Consume all keys */
}

int
cmdline_handle_key(unsigned long ksym, unsigned int state,
                   const char *buf, int len)
{
	/* Ignore bare modifier keys (Shift, Ctrl, etc.) */
	if (IsModifierKey(ksym))
		return 1;

	/* Shift+Escape: close from any state */
	if (ksym == XK_Escape && (state & ShiftMask)) {
		if (cl.prefix == '/' || cl.prefix == '?')
			cmdline_search_cancel();
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

	{
		int ret;
		if (cl.cmd_mode == 0)
			ret = cmdline_handle_insert(ksym, state, buf, len);
		else if (cl.cmd_mode == 2)
			ret = cmdline_handle_visual(ksym, state, buf, len);
		else
			ret = cmdline_handle_normal(ksym, state, buf, len);
		/* Live search: update highlights after any keypress in search mode */
		if (cl.state == CMDLINE_INPUT)
			cmdline_live_search();
		return ret;
	}
}

void
cmdline_draw(void)
{
	if (cl.state != CMDLINE_HIDDEN) {
#ifdef ST_NATIVE_MACOS
		MacCmdlineSnapshot snapshot = {
			.state = cl.state,
			.mode = cl.cmd_mode,
			.prefix = cl.prefix,
			.input = cl.input,
			.input_len = cl.input_len,
			.cursor = cl.cursor,
			.anchor = cl.anchor,
			.error = cl.errmsg,
			.x = 0,
			.y = cl.y,
			.width = cl.width,
			.height = cl.height,
			.content_y = cl.content_y,
			.content_height = cl.content_height,
			.baseline = cl.baseline,
		};
		macos_draw_cmdline_snapshot(&snapshot);
#else
		cmdline_redraw();
#endif
	}
}

void
cmdline_resize(void)
{
	int old_y = cl.y;
	int old_w = cl.width;
	int old_h = cl.height;

	cmdline_compute_geometry();

	if (cl.y != old_y || cl.width != old_w || cl.height != old_h) {
#ifndef ST_NATIVE_MACOS
		XMoveResizeWindow(xw.dpy, cl.win, 0, cl.y, cl.width, cl.height);

		if (cl.draw)
			XftDrawDestroy(cl.draw);
		if (cl.buf)
			XFreePixmap(xw.dpy, cl.buf);

		cl.buf = XCreatePixmap(xw.dpy, cl.win, cl.width, cl.height,
		                        XDefaultDepth(xw.dpy, xw.scr));
		cl.draw = XftDrawCreate(xw.dpy, cl.buf, xw.vis, xw.cmap);
#endif

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
