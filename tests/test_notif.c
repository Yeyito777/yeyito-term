/* See LICENSE for license details. */
/* Unit tests for notif module */

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
typedef struct _XGC { int dummy; } *GC;
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

/* Track X11 calls for verification */
static struct {
	int xcreategc_calls;
	int xfreegc_calls;
	int xcopyarea_calls;
	GC last_copyarea_gc;
	GC created_gc;
} x11_track;

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
static struct _XGC mock_notif_gc;
static XftDraw mock_xftdraw;
static XftFont mock_font = { .ascent = 10, .descent = 4 };
static FcPattern mock_pattern;

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
	return &mock_font;
}

void XftFontClose(Display *dpy, XftFont *font) {
	(void)dpy; (void)font;
}

void XftTextExtentsUtf8(Display *dpy, XftFont *font, const FcChar8 *string,
                        int len, XGlyphInfo *extents) {
	(void)dpy; (void)font; (void)string; (void)len;
	extents->xOff = 100;
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
	return 123;  /* Fake window ID */
}

int XDestroyWindow(Display *dpy, Window w) {
	(void)dpy; (void)w;
	return 0;
}

int XMapRaised(Display *dpy, Window w) {
	(void)dpy; (void)w;
	return 0;
}

int XMoveWindow(Display *dpy, Window w, int x, int y) {
	(void)dpy; (void)w; (void)x; (void)y;
	return 0;
}

Drawable XCreatePixmap(Display *dpy, Drawable d, unsigned int width,
                       unsigned int height, unsigned int depth) {
	(void)dpy; (void)d; (void)width; (void)height; (void)depth;
	return 456;  /* Fake pixmap ID */
}

int XFreePixmap(Display *dpy, Drawable pixmap) {
	(void)dpy; (void)pixmap;
	return 0;
}

GC XCreateGC(Display *dpy, Drawable d, unsigned long valuemask,
             XGCValues *values) {
	(void)dpy; (void)d; (void)valuemask; (void)values;
	x11_track.xcreategc_calls++;
	x11_track.created_gc = &mock_notif_gc;
	return &mock_notif_gc;
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
}

/* Initialize mock X window state */
static void
init_mock_xwindow(void)
{
	memset(&xw, 0, sizeof(xw));
	memset(&win, 0, sizeof(win));
	memset(&dc, 0, sizeof(dc));

	xw.dpy = &mock_display;
	xw.vis = &mock_visual;
	xw.scr = 0;
	xw.cmap = 0;
	xw.win = 1;

	win.w = 800;
	win.h = 600;

	/* Main terminal GC - this should NOT be used by notif */
	dc.gc = &mock_main_gc;

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
}

/* Test: notif creates its own GC */
TEST(notif_creates_own_gc)
{
	init_mock_xwindow();

	notif_show("test message");

	/* Should have created a GC */
	ASSERT_EQ(1, x11_track.xcreategc_calls);

	notif_hide();
}

/* Test: notif_draw uses its own GC, not dc.gc */
TEST(notif_draw_uses_own_gc)
{
	init_mock_xwindow();

	notif_show("test message");
	reset_x11_track();  /* Reset to track only the draw call */

	notif_draw();

	/* XCopyArea should have been called */
	ASSERT_EQ(1, x11_track.xcopyarea_calls);

	/* The GC used should NOT be dc.gc (the main terminal's GC) */
	ASSERT(x11_track.last_copyarea_gc != dc.gc);

	/* It should be the GC we created for notif */
	ASSERT(x11_track.last_copyarea_gc == &mock_notif_gc);

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
	notif.show_time.tv_sec = now.tv_sec - 1;
	notif.show_time.tv_nsec = now.tv_nsec;

	int remaining = notif_check_timeout(&now);

	/* Should have ~4000ms remaining (5000 - 1000) */
	ASSERT(remaining > 0);
	ASSERT(remaining <= notif_display_ms);

	notif_hide();
}

/* Test: notif_check_timeout returns negative when expired */
TEST(notif_check_timeout_expired)
{
	init_mock_xwindow();

	notif_show("test message");

	/* Set show_time to 6 seconds ago */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	notif.show_time.tv_sec = now.tv_sec - 6;
	notif.show_time.tv_nsec = now.tv_nsec;

	int remaining = notif_check_timeout(&now);

	/* Should be expired (negative or zero) */
	ASSERT(remaining <= 0);

	notif_hide();
}

/* Test: calling show twice cleans up first notification */
TEST(notif_replaces_existing)
{
	init_mock_xwindow();

	notif_show("first message");
	ASSERT_EQ(1, notif_active());

	/* Show again - should clean up first, then create new */
	reset_x11_track();
	notif_show("second message");

	/* Should still be active */
	ASSERT_EQ(1, notif_active());

	/* Should have freed old GC and created new one */
	ASSERT_EQ(1, x11_track.xfreegc_calls);
	ASSERT_EQ(1, x11_track.xcreategc_calls);

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
	RUN_TEST(notif_check_timeout_expired);
	RUN_TEST(notif_replaces_existing);
}

int
main(void)
{
	printf("st notif test suite\n");
	printf("========================================\n");

	RUN_SUITE(notif);

	return test_summary();
}
