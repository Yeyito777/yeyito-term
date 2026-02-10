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

/* Individual toast state */
typedef struct {
	int active;
	char msg[512];
	int line_off[NOTIF_MAX_LINES];  /* byte offset of each line in msg */
	int line_len[NOTIF_MAX_LINES];  /* byte length of each line */
	int nlines;
	Window win;
	Drawable buf;
	XftDraw *draw;
	GC gc;
	int width, height;
	struct timespec show_time;
} NotifToast;

/* Notification stack state */
static struct {
	NotifToast toasts[NOTIF_MAX_TOASTS];
	int count;
	XftFont *font;
	XftColor fg, bg, border;
	int shared_loaded;
} notif;

/* Calculate the Y position for a toast at given index in the stack */
static int
notif_toast_y(int index)
{
	int y, i;

	y = notif_margin;
	if (sshind_active())
		y += sshind_height() + notif_margin;

	for (i = 0; i < index; i++)
		y += notif.toasts[i].height + 2 * notif_border_width + notif_toast_gap;

	return y;
}

/* Parse message into lines by splitting on newlines.
 * Stores offsets and lengths (copy-safe for struct assignment). */
static void
notif_parse_lines(NotifToast *t)
{
	int off, len;
	char *nl;

	len = strlen(t->msg);
	off = 0;
	t->nlines = 0;

	while (off < len && t->nlines < NOTIF_MAX_LINES) {
		t->line_off[t->nlines] = off;
		nl = strchr(t->msg + off, '\n');
		if (nl) {
			t->line_len[t->nlines] = nl - (t->msg + off);
			*nl = '\0';
			t->nlines++;
			off = (nl - t->msg) + 1;
		} else {
			t->line_len[t->nlines] = len - off;
			t->nlines++;
			break;
		}
	}

	if (t->nlines == 0) {
		t->line_off[0] = 0;
		t->line_len[0] = 0;
		t->nlines = 1;
	}
}

/* Load shared resources (font and colors) on first toast */
static int
notif_load_shared(void)
{
	FcPattern *pattern;
	FcResult result;
	FcPattern *match;
	double fontsize;

	if (notif.shared_loaded)
		return 1;

	fontsize = usedfontsize * notif_font_scale;
	pattern = FcNameParse((const FcChar8 *)usedfont);
	if (!pattern) {
		fprintf(stderr, "notif: can't parse font pattern\n");
		return 0;
	}
	FcPatternDel(pattern, FC_PIXEL_SIZE);
	FcPatternDel(pattern, FC_SIZE);
	FcPatternAddDouble(pattern, FC_PIXEL_SIZE, fontsize);
	FcConfigSubstitute(NULL, pattern, FcMatchPattern);
	XftDefaultSubstitute(xw.dpy, xw.scr, pattern);

	match = FcFontMatch(NULL, pattern, &result);
	if (!match) {
		FcPatternDestroy(pattern);
		fprintf(stderr, "notif: can't match font\n");
		return 0;
	}

	notif.font = XftFontOpenPattern(xw.dpy, match);
	if (!notif.font) {
		FcPatternDestroy(pattern);
		FcPatternDestroy(match);
		fprintf(stderr, "notif: can't open font\n");
		return 0;
	}
	FcPatternDestroy(pattern);

	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, notif_fg_color, &notif.fg);
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, notif_bg_color, &notif.bg);
	XftColorAllocName(xw.dpy, xw.vis, xw.cmap, notif_border_color, &notif.border);

	notif.shared_loaded = 1;
	return 1;
}

/* Free shared resources when last toast is dismissed */
static void
notif_free_shared(void)
{
	if (!notif.shared_loaded)
		return;

	if (notif.font) {
		XftFontClose(xw.dpy, notif.font);
		notif.font = NULL;
	}

	XftColorFree(xw.dpy, xw.vis, xw.cmap, &notif.fg);
	XftColorFree(xw.dpy, xw.vis, xw.cmap, &notif.bg);
	XftColorFree(xw.dpy, xw.vis, xw.cmap, &notif.border);

	notif.shared_loaded = 0;
}

/* Destroy X resources for a single toast */
static void
notif_destroy_toast(NotifToast *t)
{
	if (t->draw) {
		XftDrawDestroy(t->draw);
		t->draw = NULL;
	}
	if (t->buf) {
		XFreePixmap(xw.dpy, t->buf);
		t->buf = 0;
	}
	if (t->win) {
		XDestroyWindow(xw.dpy, t->win);
		t->win = 0;
	}
	if (t->gc) {
		XFreeGC(xw.dpy, t->gc);
		t->gc = 0;
	}

	t->active = 0;
}

/* Draw a single toast */
static void
notif_draw_toast(NotifToast *t)
{
	int tx, ty, i, line_height;

	if (!t->active || !t->draw || !notif.font)
		return;

	line_height = notif.font->ascent + notif.font->descent;

	/* Clear background */
	XftDrawRect(t->draw, &notif.bg, 0, 0, t->width, t->height);

	/* Draw each line */
	tx = notif_padding + notif_border_width;
	ty = notif_padding + notif_border_width + notif.font->ascent;

	for (i = 0; i < t->nlines; i++) {
		XftDrawStringUtf8(t->draw, &notif.fg, notif.font, tx, ty,
		                  (const FcChar8 *)(t->msg + t->line_off[i]),
		                  t->line_len[i]);
		ty += line_height;
	}

	/* Copy buffer to window */
	XCopyArea(xw.dpy, t->buf, t->win, t->gc,
	          0, 0, t->width, t->height, 0, 0);
}

/* Remove a toast at given index, shift remaining, reposition */
static void
notif_remove_toast(int index)
{
	int i, x, y;

	if (index < 0 || index >= notif.count)
		return;

	if (debug_mode) {
		struct timespec now;
		double elapsed;
		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed = TIMEDIFF(now, notif.toasts[index].show_time);
		fprintf(stderr, "notif: [toast %d] hiding \"%s\" (lived %.0fms, stack: %d -> %d)\n",
		        index, notif.toasts[index].msg, elapsed,
		        notif.count, notif.count - 1);
	}

	notif_destroy_toast(&notif.toasts[index]);

	/* Shift remaining toasts up */
	for (i = index; i < notif.count - 1; i++)
		notif.toasts[i] = notif.toasts[i + 1];

	notif.count--;

	/* Clear the now-unused last slot */
	memset(&notif.toasts[notif.count], 0, sizeof(NotifToast));

	/* Reposition remaining toasts from the removed index onward */
	for (i = index; i < notif.count; i++) {
		x = win.w - notif.toasts[i].width - notif_margin;
		y = notif_toast_y(i);
		XMoveWindow(xw.dpy, notif.toasts[i].win, x, y);
	}

	/* Free shared resources if no more toasts */
	if (notif.count == 0)
		notif_free_shared();
}

/* Debug: dump the entire toast stack state */
static void
notif_debug_dump(const char *context)
{
	int i, remaining;
	struct timespec now;
	double elapsed;

	if (!debug_mode)
		return;

	clock_gettime(CLOCK_MONOTONIC, &now);

	fprintf(stderr, "notif: --- stack dump (%s) ---\n", context);
	fprintf(stderr, "notif:   count=%d, shared_loaded=%d\n",
	        notif.count, notif.shared_loaded);

	for (i = 0; i < notif.count; i++) {
		NotifToast *t = &notif.toasts[i];
		elapsed = TIMEDIFF(now, t->show_time);
		remaining = (int)(notif_display_ms - elapsed);
		fprintf(stderr, "notif:   [%d] \"%s\" (%dx%d, %d lines, %dms remaining, y=%d)\n",
		        i, t->msg, t->width, t->height, t->nlines, remaining,
		        notif_toast_y(i));
	}

	fprintf(stderr, "notif: --- end dump ---\n");
}

int
notif_active(void)
{
	return notif.count > 0;
}

int
notif_check_timeout(struct timespec *now)
{
	int i, min_remain = -1;
	double elapsed;
	int remaining;

	if (notif.count == 0)
		return -1;

	/* Iterate from end to start so removal doesn't affect lower indices */
	for (i = notif.count - 1; i >= 0; i--) {
		elapsed = TIMEDIFF((*now), notif.toasts[i].show_time);
		remaining = (int)(notif_display_ms - elapsed);
		if (remaining <= 0) {
			notif_remove_toast(i);
		} else {
			if (min_remain < 0 || remaining < min_remain)
				min_remain = remaining;
		}
	}

	return min_remain;
}

void
notif_show(const char *msg)
{
	NotifToast *t;
	XGlyphInfo extents;
	XSetWindowAttributes attrs;
	XGCValues gcvalues;
	int x, y, i, line_height, max_width;

	/* Load shared resources if needed */
	if (!notif_load_shared())
		return;

	/* If at max capacity, remove the oldest toast (last in array) */
	if (notif.count >= NOTIF_MAX_TOASTS)
		notif_remove_toast(notif.count - 1);

	/* Shift existing toasts down by one slot */
	for (i = notif.count; i > 0; i--)
		notif.toasts[i] = notif.toasts[i - 1];

	notif.count++;

	/* Initialize the new toast at index 0 */
	t = &notif.toasts[0];
	memset(t, 0, sizeof(NotifToast));
	strncpy(t->msg, msg, sizeof(t->msg) - 1);
	t->msg[sizeof(t->msg) - 1] = '\0';

	/* Parse into lines */
	notif_parse_lines(t);

	/* Measure text - find widest line */
	line_height = notif.font->ascent + notif.font->descent;
	max_width = 0;
	for (i = 0; i < t->nlines; i++) {
		XftTextExtentsUtf8(xw.dpy, notif.font,
		                   (const FcChar8 *)(t->msg + t->line_off[i]),
		                   t->line_len[i], &extents);
		if (extents.xOff > max_width)
			max_width = extents.xOff;
	}

	t->width = max_width + 2 * notif_padding + 2 * notif_border_width;
	t->height = t->nlines * line_height + 2 * notif_padding + 2 * notif_border_width;

	/* Position: top-right */
	x = win.w - t->width - notif_margin;
	y = notif_toast_y(0);

	/* Create X resources for this toast */
	attrs.background_pixel = notif.bg.pixel;
	attrs.border_pixel = notif.border.pixel;
	attrs.override_redirect = True;
	attrs.event_mask = ExposureMask;
	attrs.colormap = xw.cmap;

	t->win = XCreateWindow(xw.dpy, xw.win, x, y,
	                        t->width, t->height,
	                        notif_border_width,
	                        XDefaultDepth(xw.dpy, xw.scr),
	                        InputOutput, xw.vis,
	                        CWBackPixel | CWBorderPixel |
	                        CWOverrideRedirect | CWEventMask |
	                        CWColormap,
	                        &attrs);

	t->buf = XCreatePixmap(xw.dpy, t->win, t->width, t->height,
	                        XDefaultDepth(xw.dpy, xw.scr));

	t->draw = XftDrawCreate(xw.dpy, t->buf, xw.vis, xw.cmap);

	gcvalues.graphics_exposures = False;
	t->gc = XCreateGC(xw.dpy, t->win, GCGraphicsExposures, &gcvalues);

	XMapRaised(xw.dpy, t->win);

	/* Record show time */
	clock_gettime(CLOCK_MONOTONIC, &t->show_time);
	t->active = 1;

	if (debug_mode)
		fprintf(stderr, "notif: [toast 0] showing \"%s\" (%dx%d, %d lines, stack: %d)\n",
		        t->msg, t->width, t->height, t->nlines, notif.count);

	/* Reposition existing toasts (they've been pushed down) */
	for (i = 1; i < notif.count; i++) {
		x = win.w - notif.toasts[i].width - notif_margin;
		y = notif_toast_y(i);
		XMoveWindow(xw.dpy, notif.toasts[i].win, x, y);
	}

	/* Draw the new toast */
	notif_draw_toast(t);

	if (debug_mode)
		notif_debug_dump("after show");
}

void
notif_hide(void)
{
	int i;

	if (debug_mode && notif.count > 0)
		fprintf(stderr, "notif: hiding all %d toasts\n", notif.count);

	for (i = notif.count - 1; i >= 0; i--)
		notif_destroy_toast(&notif.toasts[i]);

	notif.count = 0;
	notif_free_shared();
}

void
notif_draw(void)
{
	int i;

	for (i = 0; i < notif.count; i++)
		notif_draw_toast(&notif.toasts[i]);
}

void
notif_resize(void)
{
	int i, x, y;

	for (i = 0; i < notif.count; i++) {
		x = win.w - notif.toasts[i].width - notif_margin;
		y = notif_toast_y(i);
		XMoveWindow(xw.dpy, notif.toasts[i].win, x, y);
	}
}
