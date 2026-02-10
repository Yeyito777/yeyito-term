/* See LICENSE for license details. */
/* Unit tests for notif module (stacked toast system) */

/* Define NOTIF_TEST to use mock X11 types instead of real ones */
#define NOTIF_TEST

/* Also define SSHIND_TEST since we include sshind.h via notif.c */
#define SSHIND_TEST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test.h"

/*
 * Mock X11/Xft types - must be defined before including notif.h
 * These shadow the real X11 types for testing purposes
 */

typedef unsigned long XID;
typedef XID Window;
typedef XID Drawable;
typedef XID Colormap;
typedef int Bool;
typedef int Status;

/* Mock struct types (need at least one member to be complete) */
typedef struct _XDisplay { int dummy; } Display;
typedef struct _Visual { int dummy; } Visual;
typedef struct _XGC { int id; } *GC;
typedef struct _XftDraw { int dummy; } XftDraw;

/* Minimal struct definitions */
typedef struct {
	unsigned long pixel;
	unsigned short red, green, blue, alpha;
} XftColor;

typedef struct {
	int ascent;
	int descent;
} XftFont;

typedef struct {
	int xOff;
} XGlyphInfo;

typedef struct { int dummy; } FcPattern;
typedef struct { int dummy; } FcResult;

/* Xlib/Xft stubs and mocks */
typedef struct {
	unsigned long background_pixel;
	unsigned long border_pixel;
	Bool override_redirect;
	long event_mask;
	Colormap colormap;
} XSetWindowAttributes;

typedef struct {
	int graphics_exposures;
} XGCValues;

typedef unsigned char FcChar8;
#define FcMatchPattern 0
#define CWBackPixel 1
#define CWBorderPixel 2
#define CWOverrideRedirect 4
#define CWEventMask 8
#define CWColormap 16
#define ExposureMask 1
#define InputOutput 1
#define GCGraphicsExposures 1
#define FC_PIXEL_SIZE "pixelsize"
#define FC_SIZE "size"
#define True 1
#define False 0

/* Include notif.h early for NOTIF_MAX_TOASTS define */
#include "../notif.h"

/* Track X11 calls for verification */
static struct {
	int xcreategc_calls;
	int xfreegc_calls;
	int xcopyarea_calls;
	int xmovewindow_calls;
	int xcreatewindow_calls;
	int xdestroywindow_calls;
	int xftfontopen_calls;
	int xftfontclose_calls;
	GC last_copyarea_gc;
} x11_track;

/* Mock GC pool - each XCreateGC returns a unique GC */
static struct _XGC mock_gcs[NOTIF_MAX_TOASTS + 2];
static int mock_gc_next;

/* Mock X window globals that notif.c expects */
typedef struct {
	int tw, th;
	int w, h;
	int ch, cw;
	int mode;
	int cursor;
} TermWindow;

typedef struct {
	Display *dpy;
	Colormap cmap;
	Window win;
	Drawable buf;
	void *specbuf;
	long xembed, wmdeletewin, netwmname, netwmiconname, netwmpid, stcwd, stnotify;
	struct {
		void *xim;
		void *xic;
		struct { short x, y; } spot;
		void *spotlist;
	} ime;
	XftDraw *draw;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	int isfixed;
	int l, t;
	int gm;
} XWindow;

typedef struct {
	XftColor *col;
	size_t collen;
	void *font, *bfont, *ifont, *ibfont;
	GC gc;
} DC;

/* Define the extern globals that notif.c references */
XWindow xw;
TermWindow win;
DC dc;
char *usedfont = "monospace";
double usedfontsize = 12.0;
int debug_mode = 0;

/* Mock X11 function implementations */
static Display mock_display;
static Visual mock_visual;
static struct _XGC mock_main_gc;
static XftDraw mock_xftdraw;
static XftFont mock_font = { .ascent = 10, .descent = 4 };
static FcPattern mock_pattern;
static int next_window_id;
static int next_pixmap_id;

FcPattern *FcNameParse(const FcChar8 *name) {
	(void)name;
	return &mock_pattern;
}

int FcPatternDel(FcPattern *p, const char *object) {
	(void)p; (void)object;
	return 1;
}

int FcPatternAddDouble(FcPattern *p, const char *object, double d) {
	(void)p; (void)object; (void)d;
	return 1;
}

int FcConfigSubstitute(void *config, FcPattern *p, int kind) {
	(void)config; (void)p; (void)kind;
	return 1;
}

void XftDefaultSubstitute(Display *dpy, int screen, FcPattern *pattern) {
	(void)dpy; (void)screen; (void)pattern;
}

FcPattern *FcFontMatch(void *config, FcPattern *p, FcResult *result) {
	(void)config; (void)p; (void)result;
	return &mock_pattern;
}

void FcPatternDestroy(FcPattern *p) {
	(void)p;
}

XftFont *XftFontOpenPattern(Display *dpy, FcPattern *pattern) {
	(void)dpy; (void)pattern;
	x11_track.xftfontopen_calls++;
	return &mock_font;
}

void XftFontClose(Display *dpy, XftFont *font) {
	(void)dpy; (void)font;
	x11_track.xftfontclose_calls++;
}

void XftTextExtentsUtf8(Display *dpy, XftFont *font, const FcChar8 *string,
                        int len, XGlyphInfo *extents) {
	(void)dpy; (void)font; (void)string;
	extents->xOff = len * 8;  /* Mock: 8 pixels per character */
}

int XftColorAllocName(Display *dpy, Visual *visual, Colormap cmap,
                      const char *name, XftColor *result) {
	(void)dpy; (void)visual; (void)cmap; (void)name;
	result->pixel = 0;
	return 1;
}

void XftColorFree(Display *dpy, Visual *visual, Colormap cmap, XftColor *color) {
	(void)dpy; (void)visual; (void)cmap; (void)color;
}

XftDraw *XftDrawCreate(Display *dpy, Drawable drawable, Visual *visual,
                       Colormap colormap) {
	(void)dpy; (void)drawable; (void)visual; (void)colormap;
	return &mock_xftdraw;
}

void XftDrawDestroy(XftDraw *draw) {
	(void)draw;
}

void XftDrawRect(XftDraw *draw, const XftColor *color, int x, int y,
                 unsigned int width, unsigned int height) {
	(void)draw; (void)color; (void)x; (void)y; (void)width; (void)height;
}

void XftDrawStringUtf8(XftDraw *draw, const XftColor *color, XftFont *font,
                       int x, int y, const FcChar8 *string, int len) {
	(void)draw; (void)color; (void)font; (void)x; (void)y; (void)string; (void)len;
}

int XDefaultDepth(Display *dpy, int screen) {
	(void)dpy; (void)screen;
	return 24;
}

/* Macro used in Xlib - redirect to our function */
#define DefaultDepth(dpy, scr) XDefaultDepth(dpy, scr)

Window XCreateWindow(Display *dpy, Window parent, int x, int y,
                     unsigned int width, unsigned int height,
                     unsigned int border_width, int depth, unsigned int class,
                     Visual *visual, unsigned long valuemask,
                     XSetWindowAttributes *attributes) {
	(void)dpy; (void)parent; (void)x; (void)y; (void)width; (void)height;
	(void)border_width; (void)depth; (void)class; (void)visual;
	(void)valuemask; (void)attributes;
	x11_track.xcreatewindow_calls++;
	return next_window_id++;
}

int XDestroyWindow(Display *dpy, Window w) {
	(void)dpy; (void)w;
	x11_track.xdestroywindow_calls++;
	return 0;
}

int XMapRaised(Display *dpy, Window w) {
	(void)dpy; (void)w;
	return 0;
}

int XMoveWindow(Display *dpy, Window w, int x, int y) {
	(void)dpy; (void)w; (void)x; (void)y;
	x11_track.xmovewindow_calls++;
	return 0;
}

Drawable XCreatePixmap(Display *dpy, Drawable d, unsigned int width,
                       unsigned int height, unsigned int depth) {
	(void)dpy; (void)d; (void)width; (void)height; (void)depth;
	return next_pixmap_id++;
}

int XFreePixmap(Display *dpy, Drawable pixmap) {
	(void)dpy; (void)pixmap;
	return 0;
}

GC XCreateGC(Display *dpy, Drawable d, unsigned long valuemask,
             XGCValues *values) {
	(void)dpy; (void)d; (void)valuemask; (void)values;
	x11_track.xcreategc_calls++;
	return &mock_gcs[mock_gc_next++];
}

int XFreeGC(Display *dpy, GC gc) {
	(void)dpy; (void)gc;
	x11_track.xfreegc_calls++;
	return 0;
}

int XCopyArea(Display *dpy, Drawable src, Drawable dest, GC gc,
              int src_x, int src_y, unsigned int width, unsigned int height,
              int dest_x, int dest_y) {
	(void)dpy; (void)src; (void)dest; (void)src_x; (void)src_y;
	(void)width; (void)height; (void)dest_x; (void)dest_y;
	x11_track.xcopyarea_calls++;
	x11_track.last_copyarea_gc = gc;
	return 0;
}

/* Mock sshind functions needed by notif.c */
int sshind_active(void) { return 0; }
int sshind_height(void) { return 0; }

/* Now include notif.c directly - it will use our mock types */
#include "../notif.c"

/* Reset tracking state */
static void
reset_x11_track(void)
{
	memset(&x11_track, 0, sizeof(x11_track));
	mock_gc_next = 0;
}

/* Initialize mock X window state */
static void
init_mock_xwindow(void)
{
	memset(&xw, 0, sizeof(xw));
	memset(&win, 0, sizeof(win));
	memset(&dc, 0, sizeof(dc));
	memset(&notif, 0, sizeof(notif));

	xw.dpy = &mock_display;
	xw.vis = &mock_visual;
	xw.scr = 0;
	xw.cmap = 0;
	xw.win = 1;

	win.w = 800;
	win.h = 600;

	/* Main terminal GC - this should NOT be used by notif */
	dc.gc = (GC)&mock_main_gc;

	next_window_id = 100;
	next_pixmap_id = 1000;

	reset_x11_track();
}

/* Test: notif_active returns correct initial state */
TEST(notif_active_initial_state)
{
	init_mock_xwindow();

	/* Should be inactive initially */
	ASSERT_EQ(0, notif_active());
}

/* Test: notif_show activates notification */
TEST(notif_show_activates)
{
	init_mock_xwindow();

	notif_show("test message");

	ASSERT_EQ(1, notif_active());
	ASSERT_EQ(1, notif.count);

	notif_hide();
}

/* Test: notif_hide deactivates notification */
TEST(notif_hide_deactivates)
{
	init_mock_xwindow();

	notif_show("test message");
	ASSERT_EQ(1, notif_active());

	notif_hide();
	ASSERT_EQ(0, notif_active());
	ASSERT_EQ(0, notif.count);
}

/* Test: notif creates its own GC per toast */
TEST(notif_creates_own_gc)
{
	init_mock_xwindow();

	notif_show("test message");

	/* Should have created exactly one GC for the toast */
	ASSERT_EQ(1, x11_track.xcreategc_calls);

	notif_hide();
}

/* Test: notif_draw uses its own GC, not dc.gc */
TEST(notif_draw_uses_own_gc)
{
	init_mock_xwindow();

	notif_show("test message");
	reset_x11_track();

	notif_draw();

	/* XCopyArea should have been called */
	ASSERT_EQ(1, x11_track.xcopyarea_calls);

	/* The GC used should NOT be dc.gc (the main terminal's GC) */
	ASSERT(x11_track.last_copyarea_gc != dc.gc);

	notif_hide();
}

/* Test: notif_hide frees the GC */
TEST(notif_hide_frees_gc)
{
	init_mock_xwindow();

	notif_show("test message");
	reset_x11_track();

	notif_hide();

	/* Should have freed the GC */
	ASSERT_EQ(1, x11_track.xfreegc_calls);
}

/* Test: notif_check_timeout returns positive when not expired */
TEST(notif_check_timeout_not_expired)
{
	init_mock_xwindow();

	notif_show("test message");

	/* Set show_time to 1 second ago */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	notif.toasts[0].show_time.tv_sec = now.tv_sec - 1;
	notif.toasts[0].show_time.tv_nsec = now.tv_nsec;

	int remaining = notif_check_timeout(&now);

	/* Should have ~4000ms remaining (5000 - 1000) */
	ASSERT(remaining > 0);
	ASSERT(remaining <= notif_display_ms);
	/* Toast should still be active */
	ASSERT_EQ(1, notif.count);

	notif_hide();
}

/* Test: notif_check_timeout removes expired toasts */
TEST(notif_check_timeout_removes_expired)
{
	init_mock_xwindow();

	notif_show("test message");

	/* Set show_time to 6 seconds ago */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	notif.toasts[0].show_time.tv_sec = now.tv_sec - 6;
	notif.toasts[0].show_time.tv_nsec = now.tv_nsec;

	int remaining = notif_check_timeout(&now);

	/* Should be expired and removed */
	ASSERT(remaining < 0);
	ASSERT_EQ(0, notif.count);
	ASSERT_EQ(0, notif_active());
}

/* Test: multiple shows create stacked toasts */
TEST(notif_stacks_multiple)
{
	init_mock_xwindow();

	notif_show("first");
	notif_show("second");

	ASSERT_EQ(2, notif.count);
	ASSERT_EQ(2, x11_track.xcreategc_calls);

	/* Newest toast is at index 0 */
	ASSERT_STR_EQ("second", notif.toasts[0].msg);
	ASSERT_STR_EQ("first", notif.toasts[1].msg);

	notif_hide();
}

/* Test: shared font is loaded once and freed once */
TEST(notif_shared_font_loaded_once)
{
	init_mock_xwindow();

	notif_show("first");
	notif_show("second");

	/* Font should be loaded exactly once (shared) */
	ASSERT_EQ(1, x11_track.xftfontopen_calls);

	notif_hide();

	/* Font should be freed exactly once */
	ASSERT_EQ(1, x11_track.xftfontclose_calls);
}

/* Test: at max capacity, oldest toast is evicted */
TEST(notif_max_evicts_oldest)
{
	int i;
	char msg[32];
	init_mock_xwindow();

	/* Fill to max capacity */
	for (i = 0; i < NOTIF_MAX_TOASTS; i++) {
		snprintf(msg, sizeof(msg), "toast %d", i);
		notif_show(msg);
	}
	ASSERT_EQ(NOTIF_MAX_TOASTS, notif.count);

	/* Show one more - should evict the oldest */
	notif_show("overflow");

	ASSERT_EQ(NOTIF_MAX_TOASTS, notif.count);
	ASSERT_STR_EQ("overflow", notif.toasts[0].msg);

	/* The oldest (toast 0) should be gone; toast 1 is now last */
	ASSERT_STR_EQ("toast 1", notif.toasts[NOTIF_MAX_TOASTS - 1].msg);

	notif_hide();
}

/* Test: multiline message gets correct height and line count */
TEST(notif_multiline_height)
{
	int line_height, expected_height;
	init_mock_xwindow();

	notif_show("line one\nline two\nline three");

	ASSERT_EQ(3, notif.toasts[0].nlines);

	/* Height should account for 3 lines */
	line_height = mock_font.ascent + mock_font.descent;
	expected_height = 3 * line_height + 2 * notif_padding + 2 * notif_border_width;
	ASSERT_EQ(expected_height, notif.toasts[0].height);

	notif_hide();
}

/* Test: expired middle toast removed, remaining repositioned */
TEST(notif_expired_middle_reposition)
{
	struct timespec now;
	init_mock_xwindow();

	notif_show("oldest");
	notif_show("middle");
	notif_show("newest");

	ASSERT_EQ(3, notif.count);

	/* Make the middle toast (index 1) expire */
	clock_gettime(CLOCK_MONOTONIC, &now);
	notif.toasts[1].show_time.tv_sec = now.tv_sec - 6;
	notif.toasts[1].show_time.tv_nsec = now.tv_nsec;

	reset_x11_track();
	notif_check_timeout(&now);

	/* Middle toast removed, count reduced */
	ASSERT_EQ(2, notif.count);
	ASSERT_STR_EQ("newest", notif.toasts[0].msg);
	ASSERT_STR_EQ("oldest", notif.toasts[1].msg);

	/* Remaining toasts repositioned */
	ASSERT(x11_track.xmovewindow_calls > 0);

	notif_hide();
}

/* Test: plain message without metadata header (backward compatible) */
TEST(notif_no_metadata_plain_msg)
{
	init_mock_xwindow();

	notif_show("plain message");

	ASSERT_EQ(1, notif.count);
	ASSERT_STR_EQ("plain message", notif.toasts[0].msg);
	ASSERT_EQ(0, notif.toasts[0].pflags);
	ASSERT_EQ(0, notif.toasts[0].timeout_ms);

	notif_hide();
}

/* Test: custom timeout parsed from metadata */
TEST(notif_custom_timeout)
{
	struct timespec now;
	int remaining;
	init_mock_xwindow();

	/* Format: t=2000\x1f\x1emessage */
	notif_show("t=2000\x1f" "\x1e" "test timeout");

	ASSERT_EQ(1, notif.count);
	ASSERT_STR_EQ("test timeout", notif.toasts[0].msg);
	ASSERT_EQ(2000, notif.toasts[0].timeout_ms);

	/* Set show_time to 1.5 seconds ago - should NOT be expired with 2s timeout */
	clock_gettime(CLOCK_MONOTONIC, &now);
	notif.toasts[0].show_time.tv_sec = now.tv_sec - 1;
	notif.toasts[0].show_time.tv_nsec = now.tv_nsec - 500000000;
	if (notif.toasts[0].show_time.tv_nsec < 0) {
		notif.toasts[0].show_time.tv_sec--;
		notif.toasts[0].show_time.tv_nsec += 1000000000;
	}

	remaining = notif_check_timeout(&now);
	ASSERT(remaining > 0);
	ASSERT_EQ(1, notif.count);

	/* Set show_time to 3 seconds ago - should be expired with 2s timeout */
	notif.toasts[0].show_time.tv_sec = now.tv_sec - 3;
	notif.toasts[0].show_time.tv_nsec = now.tv_nsec;

	remaining = notif_check_timeout(&now);
	ASSERT_EQ(0, notif.count);
}

/* Test: custom colors set pflags */
TEST(notif_custom_colors_pflags)
{
	init_mock_xwindow();

	notif_show("fg=#ff0000\x1f" "bg=#00ff00\x1f" "b=#0000ff\x1f" "\x1e" "colored");

	ASSERT_EQ(1, notif.count);
	ASSERT_STR_EQ("colored", notif.toasts[0].msg);
	ASSERT(notif.toasts[0].pflags & NOTIF_PF_FG);
	ASSERT(notif.toasts[0].pflags & NOTIF_PF_BG);
	ASSERT(notif.toasts[0].pflags & NOTIF_PF_BORDER);

	notif_hide();
}

/* Test: custom textsize opens per-toast font */
TEST(notif_custom_textsize_font)
{
	init_mock_xwindow();

	reset_x11_track();
	notif_show("ts=24\x1f" "\x1e" "big text");

	ASSERT_EQ(1, notif.count);
	ASSERT_STR_EQ("big text", notif.toasts[0].msg);
	ASSERT(notif.toasts[0].pflags & NOTIF_PF_FONT);
	ASSERT(notif.toasts[0].pfont != NULL);

	/* Should have opened 2 fonts: shared + per-toast */
	ASSERT_EQ(2, x11_track.xftfontopen_calls);

	notif_hide();

	/* Should have closed 2 fonts: per-toast + shared */
	ASSERT_EQ(2, x11_track.xftfontclose_calls);
}

/* Test: per-toast font freed on hide */
TEST(notif_pertost_resources_freed)
{
	init_mock_xwindow();

	notif_show("fg=#ff0000\x1f" "ts=20\x1f" "\x1e" "resource test");
	ASSERT(notif.toasts[0].pflags & NOTIF_PF_FG);
	ASSERT(notif.toasts[0].pflags & NOTIF_PF_FONT);

	notif_hide();

	/* After hide, all toasts destroyed, pflags should be cleared */
	ASSERT_EQ(0, notif.count);
	ASSERT_EQ(0, notif.toasts[0].pflags);
}

/* Test: multiple options combined with message */
TEST(notif_multiple_opts_combined)
{
	init_mock_xwindow();

	notif_show("t=3000\x1f" "fg=#aabbcc\x1f" "ts=16\x1f" "\x1e" "multi-opt msg");

	ASSERT_EQ(1, notif.count);
	ASSERT_STR_EQ("multi-opt msg", notif.toasts[0].msg);
	ASSERT_EQ(3000, notif.toasts[0].timeout_ms);
	ASSERT(notif.toasts[0].pflags & NOTIF_PF_FG);
	ASSERT(notif.toasts[0].pflags & NOTIF_PF_FONT);
	/* bg and border not set */
	ASSERT(!(notif.toasts[0].pflags & NOTIF_PF_BG));
	ASSERT(!(notif.toasts[0].pflags & NOTIF_PF_BORDER));

	notif_hide();
}

/* Test: metadata with only timeout, no color/font overrides */
TEST(notif_timeout_only_no_flags)
{
	init_mock_xwindow();

	notif_show("t=8000\x1f" "\x1e" "just timeout");

	ASSERT_EQ(1, notif.count);
	ASSERT_STR_EQ("just timeout", notif.toasts[0].msg);
	ASSERT_EQ(8000, notif.toasts[0].timeout_ms);
	ASSERT_EQ(0, notif.toasts[0].pflags);  /* no color/font flags */

	notif_hide();
}

/* Test suite */
TEST_SUITE(notif)
{
	RUN_TEST(notif_active_initial_state);
	RUN_TEST(notif_show_activates);
	RUN_TEST(notif_hide_deactivates);
	RUN_TEST(notif_creates_own_gc);
	RUN_TEST(notif_draw_uses_own_gc);
	RUN_TEST(notif_hide_frees_gc);
	RUN_TEST(notif_check_timeout_not_expired);
	RUN_TEST(notif_check_timeout_removes_expired);
	RUN_TEST(notif_stacks_multiple);
	RUN_TEST(notif_shared_font_loaded_once);
	RUN_TEST(notif_max_evicts_oldest);
	RUN_TEST(notif_multiline_height);
	RUN_TEST(notif_expired_middle_reposition);
	RUN_TEST(notif_no_metadata_plain_msg);
	RUN_TEST(notif_custom_timeout);
	RUN_TEST(notif_custom_colors_pflags);
	RUN_TEST(notif_custom_textsize_font);
	RUN_TEST(notif_pertost_resources_freed);
	RUN_TEST(notif_multiple_opts_combined);
	RUN_TEST(notif_timeout_only_no_flags);
}

int
main(void)
{
	printf("st notif test suite (stacked toasts)\n");
	printf("========================================\n");

	RUN_SUITE(notif);

	return test_summary();
}
