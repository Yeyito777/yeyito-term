/* See LICENSE for license details. */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/XKBlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#ifndef GLX_BACK_BUFFER_AGE_EXT
#define GLX_BACK_BUFFER_AGE_EXT 0x20F4
#endif

#define GPU_DAMAGE_HISTORY 4
#define GPU_GLYPH_HASH 8192

char *argv0;
#include "arg.h"
#include "st.h"
#include "win.h"
#include "graphics.h"
#include "persist.h"
#include "clipboard5522.h"
#include "sync.h"
#include "xstate.h"

/* types used in config.h */
typedef struct {
	uint mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Shortcut;

typedef struct {
	uint mod;
	uint button;
	void (*func)(const Arg *);
	const Arg arg;
	uint  release;
} MouseShortcut;

typedef struct {
	KeySym k;
	uint mask;
	char *s;
	/* three-valued logic variables: 0 indifferent, 1 on, -1 off */
	signed char appkey;    /* application keypad */
	signed char appcursor; /* application cursor */
} Key;

/* X modifiers */
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13|1<<14)

/* function definitions used in config.h */
static void clipcopy(const Arg *);
void clippaste(const Arg *);  /* non-static for vimnav 'p' command */
static void numlock(const Arg *);
static void selpaste(const Arg *);
static void zoom(const Arg *);
static void zoomabs(const Arg *);
static void zoomreset(const Arg *);
static void ttysend(const Arg *);

/* config.h for applying patches and the configuration. */
#include "config.h"
#include "sshind.h"
#include "notif.h"
#include "vimnav.h"
#include "cmdline.h"
#include "search.h"

/* XEMBED messages */
#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

/* macros */
#define IS_SET(flag)		((win.mode & (flag)) != 0)
#define TRUERED(x)		(((x) & 0xff0000) >> 8)
#define TRUEGREEN(x)		(((x) & 0xff00))
#define TRUEBLUE(x)		(((x) & 0xff) << 8)

static int
isfilledblock(Rune rune)
{
	return blockdraw && (rune == 0x2580 || rune == 0x2584 || rune == 0x2588);
}

typedef XftDraw *Draw;
typedef XftColor Color;
typedef XftGlyphFontSpec GlyphFontSpec;

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_DYNAMIC_DRAW
#define GL_DYNAMIC_DRAW 0x88E8
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

typedef struct {
	Atom xtarget;
	char *primary, *clipboard;
	unsigned char *primaryimage, *clipboardimage;
	size_t primaryimagelen, clipboardimagelen;
	struct timespec tclick1;
	struct timespec tclick2;
} XSelection;

typedef struct {
	Window requestor;
	Atom property, target;
	unsigned char *data;
	size_t length, offset;
	int active;
} XSelectionSend;

#define XSEL_INCR_THRESHOLD (256U * 1024U)
#define XSEL_INCR_CHUNK     (128U * 1024U)

/* Font structure */
#define Font Font_
typedef struct {
	int height;
	int width;
	int ascent;
	int descent;
	int badslant;
	int badweight;
	short lbearing;
	short rbearing;
	XftFont *match;
	FcFontSet *set;
	FcPattern *pattern;
} Font;

/* Drawing Context */
typedef struct {
	Color *col;
	size_t collen;
	Font font, bfont, ifont, ibfont;
	GC gc;
} DC;

static inline ushort sixd_to_16bit(int);
static int xmakeglyphfontspecs(XftGlyphFontSpec *, const Glyph *, int, int, int);
static void xdrawglyphfontspecs(const XftGlyphFontSpec *, Glyph, int, int, int, int);
static void xdrawglyph(Glyph, int, int);
static void xclear(int, int, int, int);
static int xgeommasktogravity(int);
static int ximopen(Display *);
static void ximinstantiate(Display *, XPointer, XPointer);
static void ximdestroy(XIM, XPointer, XPointer);
static int xicdestroy(XIC, XPointer, XPointer);
static void xinitinputcursor(void);
static void xinit(int, int);
static void cresize(int, int);
static void xresize(int, int);
static void xhints(void);
static void xensurexftbuf(int);
static void gpuinit(void);
static void gpudamageensure(void);
static void gpuresize(void);
static void gpudestroy(void);
static void gpudrawimages(int);
static void gpufreeimage(uint64_t, void *);
static void gpureleaseimages(void);
static double gpuxscale(void);
static double gpuyscale(void);
static void gpudrawline(Line, int, int, int);
static void gpudrawcell(Glyph, int, int, int, int);
static void gpudrawcursor(int, int, Glyph, int, int, Glyph);
static int xloadcolor(int, const char *, Color *);
static int xloadfont(Font *, FcPattern *);
static int xloadstylefont(int);
static void xloadfonts(const char *, double);
static void xunloadfont(Font *);
static void xunloadfonts(void);
static void xsetenv(void);
static void xseturgency(int);
static int evcol(XEvent *);
static int evrow(XEvent *);

static void expose(XEvent *);
static void visibility(XEvent *);
static void unmap(XEvent *);
static void kpress(XEvent *);
static void krelease(XEvent *);
static int xisautorepeatrelease(XEvent *);
static void cmessage(XEvent *);
static void resize(XEvent *);
static void focus(XEvent *);
static uint buttonmask(uint);
static int mouseaction(XEvent *, uint);
static void brelease(XEvent *);
static void bpress(XEvent *);
static void bmotion(XEvent *);
static void propnotify(XEvent *);
static void selnotify(XEvent *);
static void selclear_(XEvent *);
static void selrequest(XEvent *);
static void setsel(char *, Time);
static void setimage(unsigned char *, size_t, Time);
static void selsendclear(void);
static int selsendbegin(XSelectionRequestEvent *, const unsigned char *, size_t);
static int selsendpropnotify(XPropertyEvent *);
static void mousesel(XEvent *, int);
static void mousereport(XEvent *);
static char *kmap(KeySym, uint);
static int match(uint, uint);
static int clip5522_selnotify(XSelectionEvent *);
static int clip5522_propnotify(XPropertyEvent *);
static void clip5522_paste(Atom);
static void clip5522_tick(const struct timespec *);

static void run(void);
static void usage(void);

static void (*handler[LASTEvent])(XEvent *) = {
	[KeyPress] = kpress,
	[KeyRelease] = krelease,
	[ClientMessage] = cmessage,
	[ConfigureNotify] = resize,
	[VisibilityNotify] = visibility,
	[UnmapNotify] = unmap,
	[Expose] = expose,
	[FocusIn] = focus,
	[FocusOut] = focus,
	[MotionNotify] = bmotion,
	[ButtonPress] = bpress,
	[ButtonRelease] = brelease,
/*
 * Uncomment if you want the selection to disappear when you select something
 * different in another window.
 */
/*	[SelectionClear] = selclear_, */
	[SelectionNotify] = selnotify,
/*
 * PropertyNotify is only turned on when there is some INCR transfer happening
 * for the selection retrieval.
 */
	[PropertyNotify] = propnotify,
	[SelectionRequest] = selrequest,
};

/* Globals */
DC dc;
XWindow xw;          /* shared with X11 child-overlay modules via xstate.h */
static XSelection xsel;
static XSelectionSend xselsend;
TermWindow win;      /* shared with X11 child-overlay modules via xstate.h */
static SyncUpdate syncUpdate;

/* A single X selection transfer is active at once.  Keeping it streaming is
 * important: images must never pass through st's text-paste path or be held
 * wholesale in terminal memory. */
enum Clip5522Operation {
	CLIP5522_IDLE,
	CLIP5522_EVENT_TARGETS,
	CLIP5522_LIST_TARGETS,
	CLIP5522_READ,
};

typedef struct {
	enum Clip5522Operation operation;
	Atom selection;
	Atom text_target;
	char **mimes;
	size_t nmimes, next, bytes;
	int incr, sent_ok;
	struct timespec deadline;
} Clip5522Transfer;

typedef struct {
	char value[128];             /* base64 encoded, safe for OSC metadata */
	Atom selection;
	Atom text_target;
	struct timespec expiry;
	int valid;
} Clip5522Token;

static Clip5522Transfer clip5522_transfer;
static Clip5522Token clip5522_token;

#define CLIP5522_MAX_BYTES (32U * 1024U * 1024U)
#define CLIP5522_MAX_TARGETS 128
#define CLIP5522_TIMEOUT 15

/* Mouse cursors: text I-beam (default), pointer arrow, and hand (clickable) */
static Cursor xcursortext;
static Cursor xcursorpointer;
static Cursor xcursorhand;
static int xcursorsready;

/* Font Ring Cache */
enum {
	FRC_NORMAL,
	FRC_ITALIC,
	FRC_BOLD,
	FRC_ITALICBOLD
};

typedef struct {
	XftFont *font;
	int flags;
	Rune unicodep;
} Fontcache;

/* Fontcache is an array now. A new font will be appended to the array. */
static Fontcache *frc = NULL;
static int frclen = 0;
static int frccap = 0;
char *usedfont = NULL;          /* non-static for sshind.c access */
double usedfontsize = 0;        /* non-static for sshind.c access */
static double defaultfontsize = 0;

/* GPU renderer internals live under render/.  See render/gpu.c for why this
 * is included rather than compiled separately. */
#include "render/gpu.c"

static char *opt_class = NULL;
static char **opt_cmd  = NULL;
static char *opt_embed = NULL;
static char *opt_font  = NULL;
static char *opt_io    = NULL;
static char *opt_line  = NULL;
static char *opt_name  = NULL;
static char *opt_title = NULL;
static char *opt_fromsave = NULL;
static int opt_fromorphan = 0;

static uint buttons; /* bit field of pressed buttons */

static void
clip5522_write(const char *data, size_t length, void *context)
{
	(void)context;
	ttywrite(data, length, 0);
}

static void
clip5522_clear_transfer(void)
{
	size_t i;
	for (i = 0; i < clip5522_transfer.nmimes; i++)
		free(clip5522_transfer.mimes[i]);
	free(clip5522_transfer.mimes);
	memset(&clip5522_transfer, 0, sizeof(clip5522_transfer));
}

static void
clip5522_finish(void)
{
	if (clip5522_transfer.sent_ok)
		clip5522_status(clip5522_write, NULL, "DONE", 0, NULL);
	clip5522_clear_transfer();
}

static void
clip5522_error(const char *status)
{
	clip5522_status(clip5522_write, NULL, status, 0, NULL);
}

static void
clip5522_abort_read(const char *status)
{
	clip5522_error(status);
	clip5522_clear_transfer();
}

static void
clip5522_deadline(struct timespec *deadline)
{
	clock_gettime(CLOCK_MONOTONIC, deadline);
	deadline->tv_sec += CLIP5522_TIMEOUT;
}

static int
clip5522_expired(const struct timespec *now, const struct timespec *deadline)
{
	return now->tv_sec > deadline->tv_sec ||
		(now->tv_sec == deadline->tv_sec && now->tv_nsec > deadline->tv_nsec);
}

static int
clip5522_make_token(Atom selection)
{
	unsigned char raw[24];
	int fd;
	ssize_t got;
	char *encoded = NULL;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return 0;
	got = read(fd, raw, sizeof(raw));
	close(fd);
	if (got != sizeof(raw) || !clip5522_base64(raw, sizeof(raw), &encoded))
		return 0;
	if (strlen(encoded) >= sizeof(clip5522_token.value)) {
		free(encoded);
		return 0;
	}
	strcpy(clip5522_token.value, encoded);
	free(encoded);
	clip5522_token.selection = selection;
	clip5522_token.text_target = clip5522_transfer.text_target;
	clip5522_deadline(&clip5522_token.expiry);
	clip5522_token.valid = 1;
	return 1;
}

static int
clip5522_add_mime(const char *mime)
{
	size_t i;
	char **mimes;
	if (!clip5522_mime_is_valid(mime))
		return 1;
	for (i = 0; i < clip5522_transfer.nmimes; i++)
		if (!strcmp(clip5522_transfer.mimes[i], mime))
			return 1;
	if (clip5522_transfer.nmimes == CLIP5522_MAX_TARGETS)
		return 0;
	mimes = realloc(clip5522_transfer.mimes,
		(clip5522_transfer.nmimes + 1) * sizeof(*mimes));
	if (!mimes)
		return 0;
	clip5522_transfer.mimes = mimes;
	clip5522_transfer.mimes[clip5522_transfer.nmimes] = xstrdup(mime);
	clip5522_transfer.nmimes++;
	return 1;
}

static void
clip5522_send_targets(void)
{
	char *list;
	int primary = clip5522_transfer.selection == XA_PRIMARY;

	list = clip5522_join_mimes(clip5522_transfer.mimes,
		clip5522_transfer.nmimes);
	if (!list) {
		clip5522_clear_transfer();
		return;
	}
	if (clip5522_transfer.operation == CLIP5522_EVENT_TARGETS) {
		if (clip5522_make_token(clip5522_transfer.selection)) {
			clip5522_status(clip5522_write, NULL, "OK", primary,
				clip5522_token.value);
			clip5522_data(clip5522_write, NULL, ".",
				(const unsigned char *)list, strlen(list), clip5522_token.value);
			clip5522_status(clip5522_write, NULL, "DONE", 0,
				clip5522_token.value);
		}
	} else {
		clip5522_status(clip5522_write, NULL, "OK", 0, NULL);
		clip5522_data(clip5522_write, NULL, ".",
			(const unsigned char *)list, strlen(list), NULL);
		clip5522_status(clip5522_write, NULL, "DONE", 0, NULL);
	}
	free(list);
	clip5522_clear_transfer();
}

static void
clip5522_collect_targets(Atom property)
{
	Atom type;
	int format;
	unsigned long nitems, remaining, offset = 0;
	unsigned char *data = NULL;
	Atom utf8 = XInternAtom(xw.dpy, "UTF8_STRING", False);
	Atom text = XInternAtom(xw.dpy, "TEXT", False);
	Atom compound = XInternAtom(xw.dpy, "COMPOUND_TEXT", False);

	do {
		if (XGetWindowProperty(xw.dpy, xw.win, property, offset, 1024,
			False, XA_ATOM, &type, &format, &nitems, &remaining, &data) != Success ||
			type != XA_ATOM || format != 32)
			break;
		for (unsigned long i = 0; i < nitems; i++) {
			Atom atom = ((Atom *)data)[i];
			char *name = XGetAtomName(xw.dpy, atom);
			if (atom == utf8 || atom == text || atom == compound || atom == XA_STRING)
				clip5522_transfer.text_target = atom;
			if (name) {
				clip5522_add_mime(name);
				XFree(name);
			}
		}
		offset += nitems;
		XFree(data);
		data = NULL;
	} while (remaining && offset < 4096);
	if (data) XFree(data);
	XDeleteProperty(xw.dpy, xw.win, property);
	if (clip5522_transfer.text_target)
		clip5522_add_mime("text/plain");
	clip5522_send_targets();
}

static void clip5522_next(void);

static void
clip5522_consume_data(Atom property)
{
	Atom type, incr;
	int format;
	unsigned long nitems, remaining, offset = 0;
	unsigned char *data = NULL;
	incr = XInternAtom(xw.dpy, "INCR", False);

	do {
		if (XGetWindowProperty(xw.dpy, xw.win, property, offset, 1024,
			False, AnyPropertyType, &type, &format, &nitems, &remaining, &data) != Success)
			goto failed;
		if (type == incr) {
			/* The advertised INCR size is advisory, but reject huge transfers. */
			if (nitems && *(unsigned long *)data > CLIP5522_MAX_BYTES)
				goto failed;
			clip5522_transfer.incr = 1;
			XFree(data);
			XDeleteProperty(xw.dpy, xw.win, property);
			return;
		}
		if (format != 8) goto failed;
		if (nitems) {
			if (clip5522_transfer.bytes + nitems > CLIP5522_MAX_BYTES)
				goto failed;
			clip5522_data(clip5522_write, NULL,
				clip5522_transfer.mimes[clip5522_transfer.next], data, nitems, NULL);
			clip5522_transfer.bytes += nitems;
		}
		offset += nitems / 4; /* long_offset is measured in 32-bit units */
		XFree(data);
		data = NULL;
	} while (remaining && offset < CLIP5522_MAX_BYTES / 4);

	/* A zero-sized INCR property ends that target; normal properties end here. */
	if (!remaining) {
		int was_incr = clip5522_transfer.incr;
		XDeleteProperty(xw.dpy, xw.win, property);
		if (!was_incr || nitems == 0) {
			clip5522_transfer.incr = 0;
			clip5522_transfer.next++;
			clip5522_transfer.bytes = 0;
			clip5522_next();
		}
		return;
	}
	failed:
	if (data) XFree(data);
	XDeleteProperty(xw.dpy, xw.win, property);
	clip5522_abort_read("EIO");
}

static void
clip5522_next(void)
{
	Atom target;
	if (clip5522_transfer.next >= clip5522_transfer.nmimes) {
		clip5522_finish();
		return;
	}
	target = !strcmp(clip5522_transfer.mimes[clip5522_transfer.next], "text/plain") &&
		clip5522_transfer.text_target ? clip5522_transfer.text_target :
		XInternAtom(xw.dpy, clip5522_transfer.mimes[clip5522_transfer.next], False);
	if (target == None) {
		clip5522_transfer.next++;
		clip5522_next();
		return;
	}
	clip5522_deadline(&clip5522_transfer.deadline);
	XConvertSelection(xw.dpy, clip5522_transfer.selection, target, xw.clip5522,
		xw.win, CurrentTime);
}

/* Called by st.c after its OSC parser has bounded and decoded a read request. */
void
xclip5522read(const Clip5522Request *request)
{
	struct timespec now;
	size_t i;
	if (clip5522_transfer.operation != CLIP5522_IDLE) {
		clip5522_error("EBUSY");
		return;
	}
	if (request->nmimes == 1 && !strcmp(request->mimes[0], ".")) {
		clip5522_transfer.operation = CLIP5522_LIST_TARGETS;
		clip5522_transfer.selection = request->primary ? XA_PRIMARY :
			XInternAtom(xw.dpy, "CLIPBOARD", False);
		clip5522_deadline(&clip5522_transfer.deadline);
		XConvertSelection(xw.dpy, clip5522_transfer.selection,
			XInternAtom(xw.dpy, "TARGETS", False), xw.clip5522, xw.win, CurrentTime);
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!request->password || !request->paste_event_name || !clip5522_token.valid ||
		clip5522_expired(&now, &clip5522_token.expiry) ||
		strcmp(request->password, clip5522_token.value) ||
		clip5522_token.selection != (request->primary ? XA_PRIMARY :
			XInternAtom(xw.dpy, "CLIPBOARD", False))) {
		clip5522_error("EPERM");
		return;
	}
	clip5522_token.valid = 0; /* a paste event password is strictly single-use */
	clip5522_transfer.operation = CLIP5522_READ;
	clip5522_transfer.selection = request->primary ? XA_PRIMARY :
		XInternAtom(xw.dpy, "CLIPBOARD", False);
	clip5522_transfer.text_target = clip5522_token.text_target;
	for (i = 0; i < request->nmimes; i++) {
		char **mimes = realloc(clip5522_transfer.mimes,
			(clip5522_transfer.nmimes + 1) * sizeof(*mimes));
		if (!mimes) break;
		clip5522_transfer.mimes = mimes;
		clip5522_transfer.mimes[clip5522_transfer.nmimes++] = xstrdup(request->mimes[i]);
	}
	if (!clip5522_transfer.nmimes) {
		clip5522_clear_transfer();
		clip5522_error("ENOSYS");
		return;
	}
	clip5522_transfer.sent_ok = 1;
	clip5522_status(clip5522_write, NULL, "OK", 0, NULL);
	clip5522_next();
}

void
x5522paste(int primary)
{
	clip5522_paste(primary ? XA_PRIMARY : XInternAtom(xw.dpy, "CLIPBOARD", False));
}

static void
clip5522_paste(Atom selection)
{
	if (clip5522_transfer.operation != CLIP5522_IDLE)
		return;
	clip5522_transfer.operation = CLIP5522_EVENT_TARGETS;
	clip5522_transfer.selection = selection;
	clip5522_deadline(&clip5522_transfer.deadline);
	XConvertSelection(xw.dpy, selection, XInternAtom(xw.dpy, "TARGETS", False),
		xw.clip5522, xw.win, CurrentTime);
}

static int
clip5522_selnotify(XSelectionEvent *event)
{
	if (clip5522_transfer.operation == CLIP5522_IDLE ||
		(event->property != xw.clip5522 && event->property != None))
		return 0;
	if (event->property == None) {
		if (clip5522_transfer.operation == CLIP5522_READ) {
			clip5522_transfer.next++;
			clip5522_next();
		} else if (clip5522_transfer.operation == CLIP5522_EVENT_TARGETS) {
			/* A clipboard with no owner is still a paste event with no types. */
			clip5522_send_targets();
		} else if (clip5522_transfer.operation == CLIP5522_LIST_TARGETS)
			clip5522_error("ENOSYS");
		if (clip5522_transfer.operation != CLIP5522_READ &&
			clip5522_transfer.operation != CLIP5522_IDLE)
			clip5522_clear_transfer();
		return 1;
	}
	if (clip5522_transfer.operation == CLIP5522_READ)
		clip5522_consume_data(event->property);
	else
		clip5522_collect_targets(event->property);
	return 1;
}

static int
clip5522_propnotify(XPropertyEvent *event)
{
	if (clip5522_transfer.operation != CLIP5522_READ || !clip5522_transfer.incr ||
		event->atom != xw.clip5522 || event->state != PropertyNewValue)
		return 0;
	clip5522_consume_data(event->atom);
	return 1;
}

static void
clip5522_tick(const struct timespec *now)
{
	if (clip5522_token.valid && clip5522_expired(now, &clip5522_token.expiry))
		clip5522_token.valid = 0;
	if (clip5522_transfer.operation != CLIP5522_IDLE &&
		clip5522_expired(now, &clip5522_transfer.deadline)) {
		if (clip5522_transfer.operation == CLIP5522_READ)
			clip5522_abort_read("EIO");
		else if (clip5522_transfer.operation == CLIP5522_EVENT_TARGETS)
			clip5522_send_targets();
		else if (clip5522_transfer.operation == CLIP5522_LIST_TARGETS)
			clip5522_error("ENOSYS");
		if (clip5522_transfer.operation != CLIP5522_IDLE)
			clip5522_clear_transfer();
	}
}

void
clipcopy(const Arg *dummy)
{
	Atom clipboard;

	free(xsel.clipboard);
	xsel.clipboard = NULL;
	free(xsel.clipboardimage);
	xsel.clipboardimage = NULL;
	xsel.clipboardimagelen = 0;

	if (xsel.primary != NULL)
		xsel.clipboard = xstrdup(xsel.primary);
	if (xsel.primaryimage != NULL) {
		xsel.clipboardimage = xmalloc(xsel.primaryimagelen);
		memcpy(xsel.clipboardimage, xsel.primaryimage,
		    xsel.primaryimagelen);
		xsel.clipboardimagelen = xsel.primaryimagelen;
	}
	if (xsel.clipboard != NULL || xsel.clipboardimage != NULL) {
		clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
		XSetSelectionOwner(xw.dpy, clipboard, xw.win, CurrentTime);
	}
}

void
clippaste(const Arg *dummy)
{
	Atom clipboard;

	if (IS_SET(MODE_PASTEEVENT)) {
		x5522paste(0);
		return;
	}
	clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
	XConvertSelection(xw.dpy, clipboard, xsel.xtarget, clipboard,
			xw.win, CurrentTime);
}

void
selpaste(const Arg *dummy)
{
	if (IS_SET(MODE_PASTEEVENT)) {
		x5522paste(1);
		return;
	}
	XConvertSelection(xw.dpy, XA_PRIMARY, xsel.xtarget, XA_PRIMARY,
			xw.win, CurrentTime);
}

void
numlock(const Arg *dummy)
{
	win.mode ^= MODE_NUMLOCK;
}

void
zoom(const Arg *arg)
{
	Arg larg;

	larg.f = usedfontsize + arg->f;
	zoomabs(&larg);
}

void
zoomabs(const Arg *arg)
{
	if (gpu.active)
		gpudestroy();
	xunloadfonts();
	xloadfonts(usedfont, arg->f);
	if (gpudraw)
		gpuinit();
	cresize(0, 0);
	redraw();
	xhints();
}

void
zoomreset(const Arg *arg)
{
	Arg larg;

	if (defaultfontsize > 0) {
		larg.f = defaultfontsize;
		zoomabs(&larg);
	}
}

void
xsetgraphicsmode(int set)
{
	static float textchscale;
	static int active;
	Arg larg;

	set = !!set;
	if (set == active)
		return;
	if (set) {
		textchscale = chscale;
		chscale = graphicschscale;
	} else {
		chscale = textchscale;
	}
	active = set;
	larg.f = usedfontsize;
	zoomabs(&larg);
}

void
ttysend(const Arg *arg)
{
	ttywrite(arg->s, strlen(arg->s), 1);
}

int
xgpuactive(void)
{
	return gpu.active;
}

int
xgpuenabled(void)
{
	return gpudraw;
}

int
xgraphicsavailable(void)
{
	if (gpudraw && !gpu.active)
		gpuinit();
	return gpu.active;
}

void
xgetdimensions(int *width, int *height, int *cellwidth, int *cellheight)
{
	if (width) *width = win.tw;
	if (height) *height = win.th;
	if (cellwidth) *cellwidth = win.cw;
	if (cellheight) *cellheight = win.ch;
}

int
xdrawrowtop(int row)
{
	LIMIT(row, 0, MAX(0, trow() - 1));
	return gpu.active ? gpucelly(row) : borderpx + row * win.ch;
}

int
xdrawrowbottom(int row)
{
	LIMIT(row, 0, MAX(0, trow() - 1));
	return gpu.active ? gpurowbottom(row) : borderpx + (row + 1) * win.ch;
}

int
evcol(XEvent *e)
{
	int x = e->xbutton.x - borderpx;

	if (gpu.active)
		x = x / gpuxscale();
	LIMIT(x, 0, win.tw - 1);
	return x / win.cw;
}

int
evrow(XEvent *e)
{
	int y = e->xbutton.y - borderpx;

	if (gpu.active)
		y = y / gpuyscale();
	LIMIT(y, 0, win.th - 1);
	return y / win.ch;
}

void
mousesel(XEvent *e, int done)
{
	int type, seltype = SEL_REGULAR;
	unsigned char *png = NULL;
	size_t png_length = 0;
	uint state = e->xbutton.state & ~(Button1Mask | forcemousemod);

	for (type = 1; type < LEN(selmasks); ++type) {
		if (match(selmasks[type], state)) {
			seltype = type;
			break;
		}
	}
	selextend(evcol(e), evrow(e), seltype, done);
	if (done) {
		png = getselimage(&png_length);
		setsel(getsel(), e->xbutton.time);
		setimage(png, png_length, e->xbutton.time);
	}
}

void
mousereport(XEvent *e)
{
	int len, btn, code;
	int x = evcol(e), y = evrow(e);
	int state = e->xbutton.state;
	char buf[40];
	static int ox, oy;

	if (e->type == MotionNotify) {
		if (x == ox && y == oy)
			return;
		if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY))
			return;
		/* MODE_MOUSEMOTION: no reporting if no button is pressed */
		if (IS_SET(MODE_MOUSEMOTION) && buttons == 0)
			return;
		/* Set btn to lowest-numbered pressed button, or 12 if no
		 * buttons are pressed. */
		for (btn = 1; btn <= 11 && !(buttons & (1<<(btn-1))); btn++)
			;
		code = 32;
	} else {
		btn = e->xbutton.button;
		/* Only buttons 1 through 11 can be encoded */
		if (btn < 1 || btn > 11)
			return;
		if (e->type == ButtonRelease) {
			/* MODE_MOUSEX10: no button release reporting */
			if (IS_SET(MODE_MOUSEX10))
				return;
			/* Don't send release events for the scroll wheel */
			if (btn == 4 || btn == 5)
				return;
		}
		code = 0;
	}

	ox = x;
	oy = y;

	/* Encode btn into code. If no button is pressed for a motion event in
	 * MODE_MOUSEMANY, then encode it as a release. */
	if ((!IS_SET(MODE_MOUSESGR) && e->type == ButtonRelease) || btn == 12)
		code += 3;
	else if (btn >= 8)
		code += 128 + btn - 8;
	else if (btn >= 4)
		code += 64 + btn - 4;
	else
		code += btn - 1;

	if (!IS_SET(MODE_MOUSEX10)) {
		code += ((state & ShiftMask  ) ?  4 : 0)
		      + ((state & Mod1Mask   ) ?  8 : 0) /* meta key: alt */
		      + ((state & ControlMask) ? 16 : 0);
	}

	if (IS_SET(MODE_MOUSESGR)) {
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
				code, x+1, y+1,
				e->type == ButtonRelease ? 'm' : 'M');
	} else if (x < 223 && y < 223) {
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
				32+code, 32+x+1, 32+y+1);
	} else {
		return;
	}

	ttywrite(buf, len, 0);
}

uint
buttonmask(uint button)
{
	return button == Button1 ? Button1Mask
	     : button == Button2 ? Button2Mask
	     : button == Button3 ? Button3Mask
	     : button == Button4 ? Button4Mask
	     : button == Button5 ? Button5Mask
	     : 0;
}

int
mouseaction(XEvent *e, uint release)
{
	MouseShortcut *ms;

	/* ignore Button<N>mask for Button<N> - it's set on release */
	uint state = e->xbutton.state & ~buttonmask(e->xbutton.button);

	for (ms = mshortcuts; ms < mshortcuts + LEN(mshortcuts); ms++) {
		if (ms->release == release &&
		    ms->button == e->xbutton.button &&
		    (match(ms->mod, state) ||  /* exact or forced */
		     match(ms->mod, state & ~forcemousemod))) {
			ms->func(&(ms->arg));
			return 1;
		}
	}

	return 0;
}

void
bpress(XEvent *e)
{
	int btn = e->xbutton.button;
	struct timespec now;
	int snap;

	if (1 <= btn && btn <= 11)
		buttons |= 1 << (btn-1);

	if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forcemousemod)) {
		mousereport(e);
		return;
	}

	if (mouseaction(e, 0))
		return;

	if (btn == Button1) {
		/*
		 * If the user clicks below predefined timeouts specific
		 * snapping behaviour is exposed.
		 */
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (TIMEDIFF(now, xsel.tclick2) <= tripleclicktimeout) {
			snap = SNAP_LINE;
		} else if (TIMEDIFF(now, xsel.tclick1) <= doubleclicktimeout) {
			snap = SNAP_WORD;
		} else {
			snap = 0;
		}
		xsel.tclick2 = xsel.tclick1;
		xsel.tclick1 = now;

		selstart(evcol(e), evrow(e), snap);
	}
}

void
propnotify(XEvent *e)
{
	XPropertyEvent *xpev;
	Atom clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);

	xpev = &e->xproperty;
	if (selsendpropnotify(xpev))
		return;
	if (xpev->state == PropertyNewValue &&
			(xpev->atom == XA_PRIMARY ||
			 xpev->atom == clipboard)) {
		selnotify(e);
	}
	if (clip5522_propnotify(xpev))
		return;

	if (xpev->state == PropertyNewValue && xpev->atom == xw.stnotify) {
		Atom type;
		int format;
		unsigned long nitems, rem;
		unsigned char *data = NULL;
		if (XGetWindowProperty(xw.dpy, xw.win, xw.stnotify, 0, 256, True,
				XInternAtom(xw.dpy, "UTF8_STRING", False),
				&type, &format, &nitems, &rem, &data) == Success && data) {
			notif_show((char *)data);
			XFree(data);
		}
	}

	if (xpev->state == PropertyNewValue && xpev->atom == xw.stsavecmd) {
		Atom type;
		int format;
		unsigned long nitems, rem;
		unsigned char *data = NULL;
		if (XGetWindowProperty(xw.dpy, xw.win, xw.stsavecmd, 0, 256, True,
				XInternAtom(xw.dpy, "UTF8_STRING", False),
				&type, &format, &nitems, &rem, &data) == Success && data) {
			persist_set_save_cmd((char *)data);
			XFree(data);
		}
	}
}

void
selnotify(XEvent *e)
{
	ulong nitems, ofs, rem;
	int format;
	uchar *data, *last, *repl;
	Atom type, incratom, property = None;

	if (e->type == SelectionNotify && clip5522_selnotify(&e->xselection))
		return;

	incratom = XInternAtom(xw.dpy, "INCR", 0);

	ofs = 0;
	if (e->type == SelectionNotify)
		property = e->xselection.property;
	else if (e->type == PropertyNotify)
		property = e->xproperty.atom;

	if (property == None)
		return;

	do {
		if (XGetWindowProperty(xw.dpy, xw.win, property, ofs,
					BUFSIZ/4, False, AnyPropertyType,
					&type, &format, &nitems, &rem,
					&data)) {
			fprintf(stderr, "Clipboard allocation failed\n");
			return;
		}

		if (e->type == PropertyNotify && nitems == 0 && rem == 0) {
			/*
			 * If there is some PropertyNotify with no data, then
			 * this is the signal of the selection owner that all
			 * data has been transferred. PropertyChangeMask stays
			 * on for _ST_NOTIFY monitoring.
			 */
		}

		if (type == incratom) {
			/*
			 * PropertyChangeMask is always on (for _ST_NOTIFY),
			 * so we just need to delete the property to signal
			 * transfer start.
			 */
			XDeleteProperty(xw.dpy, xw.win, (int)property);
			continue;
		}

		/*
		 * As seen in getsel:
		 * Line endings are inconsistent in the terminal and GUI world
		 * copy and pasting. When receiving some selection data,
		 * replace all '\n' with '\r'.
		 * FIXME: Fix the computer world.
		 */
		repl = data;
		last = data + nitems * format / 8;
		while ((repl = memchr(repl, '\n', last - repl))) {
			*repl++ = '\r';
		}

		/*
		 * For vimnav paste, strip trailing carriage returns
		 * (which were newlines before conversion).
		 */
		if (tisvimnav_paste() && rem == 0) {
			while (last > data && *(last - 1) == '\r')
				last--;
		}

		if (IS_SET(MODE_BRCKTPASTE) && ofs == 0)
			ttywrite("\033[200~", 6, 0);
		ttywrite((char *)data, last - data, 1);
		if (IS_SET(MODE_BRCKTPASTE) && rem == 0)
			ttywrite("\033[201~", 6, 0);
		XFree(data);
		/* number of 32-bit chunks returned */
		ofs += nitems * format / 32;
	} while (rem > 0);

	/*
	 * Deleting the property again tells the selection owner to send the
	 * next data chunk in the property.
	 */
	XDeleteProperty(xw.dpy, xw.win, (int)property);

	/* Clear vimnav paste mode flag */
	vimnav_paste_done();
}

void
xclipcopy(void)
{
	clipcopy(NULL);
}

static void
selsendclear(void)
{
	if (xselsend.active && xselsend.requestor != xw.win)
		XSelectInput(xw.dpy, xselsend.requestor, NoEventMask);
	free(xselsend.data);
	memset(&xselsend, 0, sizeof(xselsend));
}

/* ICCCM INCR keeps large PNGs below the X server's maximum request size.
 * The requestor deletes the property to ask for each subsequent chunk. */
static int
selsendbegin(XSelectionRequestEvent *request, const unsigned char *data,
		size_t length)
{
	unsigned long advertised = (unsigned long)length;
	Atom incr;

	if (xselsend.active || !data || !length)
		return 0;
	xselsend.data = malloc(length);
	if (!xselsend.data)
		return 0;
	memcpy(xselsend.data, data, length);
	xselsend.requestor = request->requestor;
	xselsend.property = request->property;
	xselsend.target = request->target;
	xselsend.length = length;
	xselsend.active = 1;
	if (request->requestor != xw.win)
		XSelectInput(xw.dpy, request->requestor, PropertyChangeMask);
	incr = XInternAtom(xw.dpy, "INCR", False);
	XChangeProperty(xw.dpy, request->requestor, request->property, incr, 32,
	    PropModeReplace, (unsigned char *)&advertised, 1);
	return 1;
}

static int
selsendpropnotify(XPropertyEvent *event)
{
	size_t length;

	if (!xselsend.active || event->window != xselsend.requestor ||
	    event->atom != xselsend.property)
		return 0;
	if (event->state != PropertyDelete)
		return 1;
	if (xselsend.offset < xselsend.length) {
		length = MIN((size_t)XSEL_INCR_CHUNK,
		    xselsend.length - xselsend.offset);
		XChangeProperty(xw.dpy, xselsend.requestor, xselsend.property,
		    xselsend.target, 8, PropModeReplace,
		    xselsend.data + xselsend.offset, (int)length);
		xselsend.offset += length;
	} else {
		XChangeProperty(xw.dpy, xselsend.requestor, xselsend.property,
		    xselsend.target, 8, PropModeReplace, NULL, 0);
		selsendclear();
	}
	return 1;
}

void
selclear_(XEvent *e)
{
	selclear();
}

void
selrequest(XEvent *e)
{
	XSelectionRequestEvent *xsre;
	XSelectionEvent xev;
	Atom xa_targets, image_png, clipboard, targets[4];
	char *seltext;
	unsigned char *selimage;
	size_t selimagelen;

	xsre = (XSelectionRequestEvent *) e;
	xev.type = SelectionNotify;
	xev.requestor = xsre->requestor;
	xev.selection = xsre->selection;
	xev.target = xsre->target;
	xev.time = xsre->time;
	if (xsre->property == None)
		xsre->property = xsre->target;

	/* reject */
	xev.property = None;

	xa_targets = XInternAtom(xw.dpy, "TARGETS", 0);
	image_png = XInternAtom(xw.dpy, "image/png", 0);
	clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
	if (xsre->selection == XA_PRIMARY) {
		seltext = xsel.primary;
		selimage = xsel.primaryimage;
		selimagelen = xsel.primaryimagelen;
	} else if (xsre->selection == clipboard) {
		seltext = xsel.clipboard;
		selimage = xsel.clipboardimage;
		selimagelen = xsel.clipboardimagelen;
	} else {
		fprintf(stderr, "Unhandled clipboard selection 0x%lx\n",
		    xsre->selection);
		return;
	}
	if (xsre->target == xa_targets) {
		int count = 0;
		targets[count++] = xa_targets;
		if (seltext) {
			targets[count++] = xsel.xtarget;
			targets[count++] = XA_STRING;
		}
		if (selimage)
			targets[count++] = image_png;
		XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				XA_ATOM, 32, PropModeReplace,
				(uchar *)targets, count);
		xev.property = xsre->property;
	} else if (xsre->target == xsel.xtarget || xsre->target == XA_STRING) {
		/*
		 * xith XA_STRING non ascii characters may be incorrect in the
		 * requestor. It is not our problem, use utf8.
		 */
		if (seltext != NULL) {
			XChangeProperty(xsre->display, xsre->requestor,
					xsre->property, xsre->target,
					8, PropModeReplace,
					(uchar *)seltext, strlen(seltext));
				xev.property = xsre->property;
		}
	} else if (xsre->target == image_png && selimage != NULL) {
		if (selimagelen <= XSEL_INCR_THRESHOLD) {
			XChangeProperty(xsre->display, xsre->requestor, xsre->property,
			    image_png, 8, PropModeReplace, selimage, (int)selimagelen);
			xev.property = xsre->property;
		} else if (selsendbegin(xsre, selimage, selimagelen)) {
			xev.property = xsre->property;
		}
	}

	/* all done, send a notification to the listener */
	if (!XSendEvent(xsre->display, xsre->requestor, 1, 0, (XEvent *) &xev))
		fprintf(stderr, "Error sending SelectionNotify event\n");
}

void
setsel(char *str, Time t)
{
	if (!str)
		return;

	free(xsel.primary);
	xsel.primary = str;
	free(xsel.primaryimage);
	xsel.primaryimage = NULL;
	xsel.primaryimagelen = 0;

	XSetSelectionOwner(xw.dpy, XA_PRIMARY, xw.win, t);
	if (XGetSelectionOwner(xw.dpy, XA_PRIMARY) != xw.win)
		selclear();
}

void
xsetsel(char *str)
{
	setsel(str, CurrentTime);
}

static void
setimage(unsigned char *png, size_t length, Time t)
{
	free(xsel.primaryimage);
	xsel.primaryimage = png;
	xsel.primaryimagelen = png ? length : 0;
	if (png) {
		XSetSelectionOwner(xw.dpy, XA_PRIMARY, xw.win, t);
		if (XGetSelectionOwner(xw.dpy, XA_PRIMARY) != xw.win)
			selclear();
	}
}

void
xsetimage(unsigned char *png, size_t length)
{
	setimage(png, length, CurrentTime);
}

void
brelease(XEvent *e)
{
	int btn = e->xbutton.button;

	if (1 <= btn && btn <= 11)
		buttons &= ~(1 << (btn-1));

	if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forcemousemod)) {
		mousereport(e);
		return;
	}

	if (mouseaction(e, 1))
		return;
	if (btn == Button1)
		mousesel(e, 1);
}

void
bmotion(XEvent *e)
{
	if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forcemousemod)) {
		mousereport(e);
		return;
	}

	mousesel(e, 0);
}

void
cresize(int width, int height)
{
	int col, row;

	if (width != 0)
		win.w = width;
	if (height != 0)
		win.h = height;

	col = (win.w - 2 * borderpx) / win.cw;
	row = (win.h - 2 * borderpx) / win.ch;
	col = MAX(1, col);
	row = MAX(1, row);

	tresize(col, row);
	xresize(col, row);
	ttyresize(win.tw, win.th);
}

void
xresize(int col, int row)
{
	win.tw = col * win.cw;
	win.th = row * win.ch;

	if (xw.buf) {
		XFreePixmap(xw.dpy, xw.buf);
		xw.buf = XCreatePixmap(xw.dpy, xw.win, win.w, win.h,
				DefaultDepth(xw.dpy, xw.scr));
		if (xw.draw)
			XftDrawChange(xw.draw, xw.buf);
		xclear(0, 0, win.w, win.h);
	}
	gpuresize();

	/* resize to new width */
	if (xw.specbuf)
		xw.specbuf = xrealloc(xw.specbuf, col * sizeof(GlyphFontSpec));
}

static void
xensurexftbuf(int col)
{
	if (!xw.buf) {
		xw.buf = XCreatePixmap(xw.dpy, xw.win, win.w, win.h,
				DefaultDepth(xw.dpy, xw.scr));
		XSetForeground(xw.dpy, dc.gc, dc.col[defaultbg].pixel);
		XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, win.w, win.h);
	}
	if (!xw.draw)
		xw.draw = XftDrawCreate(xw.dpy, xw.buf, xw.vis, xw.cmap);
	if (!xw.specbuf)
		xw.specbuf = xmalloc(col * sizeof(GlyphFontSpec));
}

static void
xinitinputcursor(void)
{
	XColor xmousefg, xmousebg;

	if (xcursorsready)
		return;

	/* white cursor, black outline */
	xcursortext = XCreateFontCursor(xw.dpy, mouseshape);
	xcursorpointer = XCreateFontCursor(xw.dpy, mousecursorshape);
	xcursorhand = XCreateFontCursor(xw.dpy, mousehandshape);

	if (XParseColor(xw.dpy, xw.cmap, colorname[mousefg], &xmousefg) == 0) {
		xmousefg.red   = 0xffff;
		xmousefg.green = 0xffff;
		xmousefg.blue  = 0xffff;
	}

	if (XParseColor(xw.dpy, xw.cmap, colorname[mousebg], &xmousebg) == 0) {
		xmousebg.red   = 0x0000;
		xmousebg.green = 0x0000;
		xmousebg.blue  = 0x0000;
	}

	XRecolorCursor(xw.dpy, xcursortext, &xmousefg, &xmousebg);
	XRecolorCursor(xw.dpy, xcursorpointer, &xmousefg, &xmousebg);
	XRecolorCursor(xw.dpy, xcursorhand, &xmousefg, &xmousebg);
	xcursorsready = 1;
}

ushort
sixd_to_16bit(int x)
{
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

int
xloadcolor(int i, const char *name, Color *ncolor)
{
	XRenderColor color = { .alpha = 0xffff };

	if (!name) {
		if (BETWEEN(i, 16, 255)) { /* 256 color */
			if (i < 6*6*6+16) { /* same colors as xterm */
				color.red   = sixd_to_16bit( ((i-16)/36)%6 );
				color.green = sixd_to_16bit( ((i-16)/6) %6 );
				color.blue  = sixd_to_16bit( ((i-16)/1) %6 );
			} else { /* greyscale */
				color.red = 0x0808 + 0x0a0a * (i - (6*6*6+16));
				color.green = color.blue = color.red;
			}
			return XftColorAllocValue(xw.dpy, xw.vis,
			                          xw.cmap, &color, ncolor);
		} else
			name = colorname[i];
	}

	return XftColorAllocName(xw.dpy, xw.vis, xw.cmap, name, ncolor);
}

void
xloadcols(void)
{
	int i;
	static int loaded;
	Color *cp;

	if (loaded) {
		for (cp = dc.col; cp < &dc.col[dc.collen]; ++cp)
			XftColorFree(xw.dpy, xw.vis, xw.cmap, cp);
	} else {
		dc.collen = MAX(LEN(colorname), 256);
		dc.col = xmalloc(dc.collen * sizeof(Color));
	}

	for (i = 0; i < dc.collen; i++)
		if (!xloadcolor(i, NULL, &dc.col[i])) {
			if (colorname[i])
				die("could not allocate color '%s'\n", colorname[i]);
			else
				die("could not allocate color %d\n", i);
		}
	loaded = 1;
	gpupalvalid = 0;
}

int
xgetcolor(int x, unsigned char *r, unsigned char *g, unsigned char *b)
{
	if (!BETWEEN(x, 0, dc.collen - 1))
		return 1;

	*r = dc.col[x].color.red >> 8;
	*g = dc.col[x].color.green >> 8;
	*b = dc.col[x].color.blue >> 8;

	return 0;
}

int
xsetcolorname(int x, const char *name)
{
	Color ncolor;

	if (!BETWEEN(x, 0, dc.collen - 1))
		return 1;

	if (!xloadcolor(x, name, &ncolor))
		return 1;

	XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.col[x]);
	dc.col[x] = ncolor;
	gpupalvalid = 0;

	return 0;
}

/*
 * Absolute coordinates.
 */
void
xclear(int x1, int y1, int x2, int y2)
{
	XftDrawRect(xw.draw,
			&dc.col[IS_SET(MODE_REVERSE)? defaultfg : defaultbg],
			x1, y1, x2-x1, y2-y1);
}

void
xhints(void)
{
	XClassHint class = {opt_name ? opt_name : termname,
	                    opt_class ? opt_class : termname};
	XWMHints wm = {.flags = InputHint, .input = 1};
	XSizeHints *sizeh;

	sizeh = XAllocSizeHints();

	sizeh->flags = PSize | PResizeInc | PBaseSize | PMinSize;
	sizeh->height = win.h;
	sizeh->width = win.w;
	/*
	 * The GPU renderer supports fractional cell scaling by keeping the terminal
	 * grid fixed and stretching cells to the actual pixel size.  GPU startup is
	 * lazy, so gpu.active is still false when the window is first mapped; using
	 * the Xft cell increments here makes the WM snap the initial tiled size down
	 * to an integer grid until the user manually resizes.  Advertise pixel resize
	 * increments as soon as the GPU path is configured, not only after GL init.
	 */
	sizeh->height_inc = gpudraw ? 1 : win.ch;
	sizeh->width_inc = gpudraw ? 1 : win.cw;
	sizeh->base_height = 2 * borderpx;
	sizeh->base_width = 2 * borderpx;
	sizeh->min_height = win.ch + 2 * borderpx;
	sizeh->min_width = win.cw + 2 * borderpx;
	if (xw.isfixed) {
		sizeh->flags |= PMaxSize;
		sizeh->min_width = sizeh->max_width = win.w;
		sizeh->min_height = sizeh->max_height = win.h;
	}
	if (xw.gm & (XValue|YValue)) {
		sizeh->flags |= USPosition | PWinGravity;
		sizeh->x = xw.l;
		sizeh->y = xw.t;
		sizeh->win_gravity = xgeommasktogravity(xw.gm);
	}

	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm,
			&class);
	XFree(sizeh);
}

int
xgeommasktogravity(int mask)
{
	switch (mask & (XNegative|YNegative)) {
	case 0:
		return NorthWestGravity;
	case XNegative:
		return NorthEastGravity;
	case YNegative:
		return SouthWestGravity;
	}

	return SouthEastGravity;
}

int
xloadfont(Font *f, FcPattern *pattern)
{
	FcPattern *configured;
	FcPattern *match;
	FcResult result;
	XGlyphInfo extents;
	int wantattr, haveattr;

	memset(f, 0, sizeof *f);

	/*
	 * Manually configure instead of calling XftMatchFont
	 * so that we can use the configured pattern for
	 * "missing glyph" lookups.
	 */
	configured = FcPatternDuplicate(pattern);
	if (!configured)
		return 1;

	FcConfigSubstitute(NULL, configured, FcMatchPattern);
	XftDefaultSubstitute(xw.dpy, xw.scr, configured);

	match = FcFontMatch(NULL, configured, &result);
	if (!match) {
		FcPatternDestroy(configured);
		return 1;
	}

	if (!(f->match = XftFontOpenPattern(xw.dpy, match))) {
		FcPatternDestroy(configured);
		FcPatternDestroy(match);
		return 1;
	}

	if ((XftPatternGetInteger(pattern, "slant", 0, &wantattr) ==
	    XftResultMatch)) {
		/*
		 * Check if xft was unable to find a font with the appropriate
		 * slant but gave us one anyway. Try to mitigate.
		 */
		if ((XftPatternGetInteger(f->match->pattern, "slant", 0,
		    &haveattr) != XftResultMatch) || haveattr < wantattr) {
			f->badslant = 1;
			fputs("font slant does not match\n", stderr);
		}
	}

	if ((XftPatternGetInteger(pattern, "weight", 0, &wantattr) ==
	    XftResultMatch)) {
		if ((XftPatternGetInteger(f->match->pattern, "weight", 0,
		    &haveattr) != XftResultMatch) || haveattr != wantattr) {
			f->badweight = 1;
			fputs("font weight does not match\n", stderr);
		}
	}

	XftTextExtentsUtf8(xw.dpy, f->match,
		(const FcChar8 *) ascii_printable,
		strlen(ascii_printable), &extents);

	f->set = NULL;
	f->pattern = configured;

	f->ascent = f->match->ascent;
	f->descent = f->match->descent;
	f->lbearing = 0;
	f->rbearing = f->match->max_advance_width;

	f->height = f->ascent + f->descent;
	f->width = DIVCEIL(extents.xOff, strlen(ascii_printable));

	return 0;
}

static int
xloadstylefont(int style)
{
	FcPattern *pattern;
	Font *fontp;

	if (!gpudraw || !usedfont)
		return 1;
	if (style == FRC_BOLD) {
		fontp = &dc.bfont;
		if (fontp->match)
			return 0;
	} else if (style == FRC_ITALIC) {
		fontp = &dc.ifont;
		if (fontp->match)
			return 0;
	} else if (style == FRC_ITALICBOLD) {
		fontp = &dc.ibfont;
		if (fontp->match)
			return 0;
	} else {
		return 1;
	}

	pattern = usedfont[0] == '-' ?
	          XftXlfdParse(usedfont, False, False) :
	          FcNameParse((const FcChar8 *)usedfont);
	if (!pattern)
		return 1;
	if (usedfontsize > 0) {
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternDel(pattern, FC_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE, usedfontsize);
	}
	if (style == FRC_ITALIC || style == FRC_ITALICBOLD) {
		FcPatternDel(pattern, FC_SLANT);
		FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	}
	if (style == FRC_BOLD || style == FRC_ITALICBOLD) {
		FcPatternDel(pattern, FC_WEIGHT);
		FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	}
	style = xloadfont(fontp, pattern);
	FcPatternDestroy(pattern);
	return style;
}

void
xloadfonts(const char *fontstr, double fontsize)
{
	FcPattern *pattern;
	double fontval;

	if (fontstr[0] == '-')
		pattern = XftXlfdParse(fontstr, False, False);
	else
		pattern = FcNameParse((const FcChar8 *)fontstr);

	if (!pattern)
		die("can't open font %s\n", fontstr);

	if (fontsize > 1) {
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternDel(pattern, FC_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
		usedfontsize = fontsize;
	} else {
		if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval) ==
				FcResultMatch) {
			usedfontsize = fontval;
		} else if (FcPatternGetDouble(pattern, FC_SIZE, 0, &fontval) ==
				FcResultMatch) {
			usedfontsize = -1;
		} else {
			/*
			 * Default font size is 12, if none given. This is to
			 * have a known usedfontsize value.
			 */
			FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
			usedfontsize = 12;
		}
		defaultfontsize = usedfontsize;
	}

	if (xloadfont(&dc.font, pattern))
		die("can't open font %s\n", fontstr);

	if (usedfontsize < 0) {
		FcPatternGetDouble(dc.font.match->pattern,
		                   FC_PIXEL_SIZE, 0, &fontval);
		usedfontsize = fontval;
		if (fontsize == 0)
			defaultfontsize = fontval;
	}

	/* Setting character width and height. */
	win.cw = ceilf(dc.font.width * cwscale);
	win.ch = ceilf(dc.font.height * chscale);
	if (gpudraw) {
		FcPatternDestroy(pattern);
		return;
	}

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	if (xloadfont(&dc.ifont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	if (xloadfont(&dc.ibfont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	if (xloadfont(&dc.bfont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDestroy(pattern);
}

void
xunloadfont(Font *f)
{
	if (!f->match)
		return;
	XftFontClose(xw.dpy, f->match);
	FcPatternDestroy(f->pattern);
	if (f->set)
		FcFontSetDestroy(f->set);
	memset(f, 0, sizeof *f);
}

void
xunloadfonts(void)
{
	/* Free the loaded fonts in the font cache.  */
	while (frclen > 0)
		XftFontClose(xw.dpy, frc[--frclen].font);

	xunloadfont(&dc.font);
	xunloadfont(&dc.bfont);
	xunloadfont(&dc.ifont);
	xunloadfont(&dc.ibfont);
}

int
ximopen(Display *dpy)
{
	XIMCallback imdestroy = { .client_data = NULL, .callback = ximdestroy };
	XICCallback icdestroy = { .client_data = NULL, .callback = xicdestroy };

	xw.ime.xim = XOpenIM(xw.dpy, NULL, NULL, NULL);
	if (xw.ime.xim == NULL)
		return 0;

	if (XSetIMValues(xw.ime.xim, XNDestroyCallback, &imdestroy, NULL))
		fprintf(stderr, "XSetIMValues: "
		                "Could not set XNDestroyCallback.\n");

	xw.ime.spotlist = XVaCreateNestedList(0, XNSpotLocation, &xw.ime.spot,
	                                      NULL);

	if (xw.ime.xic == NULL) {
		xw.ime.xic = XCreateIC(xw.ime.xim, XNInputStyle,
		                       XIMPreeditNothing | XIMStatusNothing,
		                       XNClientWindow, xw.win,
		                       XNDestroyCallback, &icdestroy,
		                       NULL);
	}
	if (xw.ime.xic == NULL)
		fprintf(stderr, "XCreateIC: Could not create input context.\n");

	return 1;
}

void
ximinstantiate(Display *dpy, XPointer client, XPointer call)
{
	if (ximopen(dpy))
		XUnregisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
		                                 ximinstantiate, NULL);
}

void
ximdestroy(XIM xim, XPointer client, XPointer call)
{
	xw.ime.xim = NULL;
	XRegisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
	                               ximinstantiate, NULL);
	XFree(xw.ime.spotlist);
}

int
xicdestroy(XIC xim, XPointer client, XPointer call)
{
	xw.ime.xic = NULL;
	return 1;
}

static void
xinitatoms(pid_t thispid)
{
	xw.xembed = XInternAtom(xw.dpy, "_XEMBED", False);
	xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
	xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
	xw.netwmiconname = XInternAtom(xw.dpy, "_NET_WM_ICON_NAME", False);
	XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

	xw.netwmpid = XInternAtom(xw.dpy, "_NET_WM_PID", False);
	XChangeProperty(xw.dpy, xw.win, xw.netwmpid, XA_CARDINAL, 32,
			PropModeReplace, (uchar *)&thispid, 1);

	xw.stcwd = XInternAtom(xw.dpy, "_ST_CWD", False);
	xw.stnotify = XInternAtom(xw.dpy, "_ST_NOTIFY", False);
	xw.stsavecmd = XInternAtom(xw.dpy, "_ST_SAVE_CMD", False);
	xw.clip5522 = XInternAtom(xw.dpy, "_ST_CLIPBOARD_5522", False);
}

void
xinit(int cols, int rows)
{
	XGCValues gcvalues;
	Window parent, root;
	pid_t thispid = getpid();

	if (!(xw.dpy = XOpenDisplay(NULL)))
		die("can't open display\n");
	if (!gpudraw) {
		Bool detectable_autorepeat_supported;
		XkbSetDetectableAutoRepeat(xw.dpy, True, &detectable_autorepeat_supported);
	}
	xw.scr = XDefaultScreen(xw.dpy);
	xw.vis = XDefaultVisual(xw.dpy, xw.scr);

	/* font */
	if (!FcInit())
		die("could not init fontconfig.\n");

	usedfont = (opt_font == NULL)? font : opt_font;
	xloadfonts(usedfont, 0);

	/* colors */
	xw.cmap = XDefaultColormap(xw.dpy, xw.scr);
	xloadcols();

	/* adjust fixed window geometry */
	win.w = 2 * borderpx + cols * win.cw;
	win.h = 2 * borderpx + rows * win.ch;
	if (xw.gm & XNegative)
		xw.l += DisplayWidth(xw.dpy, xw.scr) - win.w - 2;
	if (xw.gm & YNegative)
		xw.t += DisplayHeight(xw.dpy, xw.scr) - win.h - 2;

	/* Events */
	xw.attrs.background_pixel = dc.col[defaultbg].pixel;
	xw.attrs.border_pixel = dc.col[defaultbg].pixel;
	xw.attrs.bit_gravity = NorthWestGravity;
	xw.attrs.event_mask = FocusChangeMask | KeyPressMask | KeyReleaseMask
		| ExposureMask | VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask
		| PropertyChangeMask;
	xw.attrs.colormap = xw.cmap;

	root = XRootWindow(xw.dpy, xw.scr);
	if (!(opt_embed && (parent = strtol(opt_embed, NULL, 0))))
		parent = root;
	xw.win = XCreateWindow(xw.dpy, root, xw.l, xw.t,
			win.w, win.h, 0, XDefaultDepth(xw.dpy, xw.scr), InputOutput,
			xw.vis, CWBackPixel | CWBorderPixel | CWBitGravity
			| CWEventMask | CWColormap, &xw.attrs);
	if (parent != root)
		XReparentWindow(xw.dpy, xw.win, parent, xw.l, xw.t);

	memset(&gcvalues, 0, sizeof(gcvalues));
	gcvalues.graphics_exposures = False;
	dc.gc = XCreateGC(xw.dpy, xw.win, GCGraphicsExposures,
			&gcvalues);
	if (!gpudraw)
		xensurexftbuf(cols);

	/* input methods */
	if (!ximopen(xw.dpy)) {
		XRegisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
	                                       ximinstantiate, NULL);
	}

	if (!gpudraw) {
		xinitinputcursor();
		XDefineCursor(xw.dpy, xw.win, xcursortext);
		xinitatoms(thispid);
	}

	win.mode = MODE_NUMLOCK | MODE_ONSCREEN;
	if (gpudraw) {
		xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
		xw.netwmiconname = XInternAtom(xw.dpy, "_NET_WM_ICON_NAME", False);
	}
	resettitle();
	xhints();
	XMapWindow(xw.dpy, xw.win);
	XFlush(xw.dpy);
	if (gpudraw)
		xinitatoms(thispid);

	clock_gettime(CLOCK_MONOTONIC, &xsel.tclick1);
	clock_gettime(CLOCK_MONOTONIC, &xsel.tclick2);
	xsel.primary = NULL;
	xsel.clipboard = NULL;
	xsel.primaryimage = NULL;
	xsel.clipboardimage = NULL;
	xsel.primaryimagelen = 0;
	xsel.clipboardimagelen = 0;
	xsel.xtarget = XInternAtom(xw.dpy, "UTF8_STRING", 0);
	if (xsel.xtarget == None)
		xsel.xtarget = XA_STRING;
	graphics_set_image_free_callback(gpufreeimage, NULL);
}

int
xmakeglyphfontspecs(XftGlyphFontSpec *specs, const Glyph *glyphs, int len, int x, int y)
{
	float winx = borderpx + x * win.cw, winy = borderpx + y * win.ch, xp, yp;
	ushort mode, prevmode = USHRT_MAX;
	Font *font = &dc.font;
	int frcflags = FRC_NORMAL;
	float runewidth = win.cw;
	Rune rune;
	FT_UInt glyphidx;
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[] = { NULL };
	FcCharSet *fccharset;
	int i, f, numspecs = 0;

	for (i = 0, xp = winx, yp = winy + font->ascent; i < len; ++i) {
		/* Fetch rune and mode for current glyph. */
		rune = glyphs[i].u;
		mode = glyphs[i].mode;

		/* Skip dummy wide-character spacing. */
		if (mode == ATTR_WDUMMY)
			continue;

		/* Determine font for glyph if different from previous glyph. */
		if (prevmode != mode) {
			prevmode = mode;
			font = &dc.font;
			frcflags = FRC_NORMAL;
			runewidth = win.cw * ((mode & ATTR_WIDE) ? 2.0f : 1.0f);
			if ((mode & ATTR_ITALIC) && (mode & ATTR_BOLD)) {
				font = &dc.ibfont;
				frcflags = FRC_ITALICBOLD;
			} else if (mode & ATTR_ITALIC) {
				font = &dc.ifont;
				frcflags = FRC_ITALIC;
			} else if (mode & ATTR_BOLD) {
				font = &dc.bfont;
				frcflags = FRC_BOLD;
			}
			yp = winy + font->ascent;
		}

		/* Lookup character index with default font. */
		glyphidx = XftCharIndex(xw.dpy, font->match, rune);
		if (glyphidx) {
			specs[numspecs].font = font->match;
			specs[numspecs].glyph = glyphidx;
			specs[numspecs].x = (short)xp;
			specs[numspecs].y = (short)yp;
			xp += runewidth;
			numspecs++;
			continue;
		}

		/* Fallback on font cache, search the font cache for match. */
		for (f = 0; f < frclen; f++) {
			glyphidx = XftCharIndex(xw.dpy, frc[f].font, rune);
			/* Everything correct. */
			if (glyphidx && frc[f].flags == frcflags)
				break;
			/* We got a default font for a not found glyph. */
			if (!glyphidx && frc[f].flags == frcflags
					&& frc[f].unicodep == rune) {
				break;
			}
		}

		/* Nothing was found. Use fontconfig to find matching font. */
		if (f >= frclen) {
			if (!font->set)
				font->set = FcFontSort(0, font->pattern,
				                       1, 0, &fcres);
			fcsets[0] = font->set;

			/*
			 * Nothing was found in the cache. Now use
			 * some dozen of Fontconfig calls to get the
			 * font for one single character.
			 *
			 * Xft and fontconfig are design failures.
			 */
			fcpattern = FcPatternDuplicate(font->pattern);
			fccharset = FcCharSetCreate();

			FcCharSetAddChar(fccharset, rune);
			FcPatternAddCharSet(fcpattern, FC_CHARSET,
					fccharset);
			FcPatternAddBool(fcpattern, FC_SCALABLE, 1);

			FcConfigSubstitute(0, fcpattern,
					FcMatchPattern);
			FcDefaultSubstitute(fcpattern);

			fontpattern = FcFontSetMatch(0, fcsets, 1,
					fcpattern, &fcres);

			/* Allocate memory for the new cache entry. */
			if (frclen >= frccap) {
				frccap += 16;
				frc = xrealloc(frc, frccap * sizeof(Fontcache));
			}

			frc[frclen].font = XftFontOpenPattern(xw.dpy,
					fontpattern);
			if (!frc[frclen].font)
				die("XftFontOpenPattern failed seeking fallback font: %s\n",
					strerror(errno));
			frc[frclen].flags = frcflags;
			frc[frclen].unicodep = rune;

			glyphidx = XftCharIndex(xw.dpy, frc[frclen].font, rune);

			f = frclen;
			frclen++;

			FcPatternDestroy(fcpattern);
			FcCharSetDestroy(fccharset);
		}

		specs[numspecs].font = frc[f].font;
		specs[numspecs].glyph = glyphidx;
		specs[numspecs].x = (short)xp;
		specs[numspecs].y = (short)yp;
		xp += runewidth;
		numspecs++;
	}

	return numspecs;
}

void
xdrawglyphfontspecs(const XftGlyphFontSpec *specs, Glyph base, int len, int x, int y, int dmode)
{
	int charlen = len * ((base.mode & ATTR_WIDE) ? 2 : 1);
	int winx = borderpx + x * win.cw, winy = borderpx + y * win.ch,
	    width = charlen * win.cw;
	Color *fg, *bg, *temp, revfg, revbg, truefg, truebg;
	XRenderColor colfg, colbg;
	XRectangle r;

	/* Fallback on color display for attributes not supported by the font */
	if (base.mode & ATTR_ITALIC && base.mode & ATTR_BOLD) {
		if (dc.ibfont.badslant || dc.ibfont.badweight)
			base.fg = defaultattr;
	} else if ((base.mode & ATTR_ITALIC && dc.ifont.badslant) ||
	    (base.mode & ATTR_BOLD && dc.bfont.badweight)) {
		base.fg = defaultattr;
	}

	if (IS_TRUECOL(base.fg)) {
		colfg.alpha = 0xffff;
		colfg.red = TRUERED(base.fg);
		colfg.green = TRUEGREEN(base.fg);
		colfg.blue = TRUEBLUE(base.fg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &truefg);
		fg = &truefg;
	} else {
		fg = &dc.col[base.fg];
	}

	if (IS_TRUECOL(base.bg)) {
		colbg.alpha = 0xffff;
		colbg.green = TRUEGREEN(base.bg);
		colbg.red = TRUERED(base.bg);
		colbg.blue = TRUEBLUE(base.bg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &truebg);
		bg = &truebg;
	} else {
		bg = &dc.col[base.bg];
	}

	/* Change basic system colors [0-7] to bright system colors [8-15] */
	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_BOLD && BETWEEN(base.fg, 0, 7))
		fg = &dc.col[base.fg + 8];

	if (IS_SET(MODE_REVERSE)) {
		if (fg == &dc.col[defaultfg]) {
			fg = &dc.col[defaultbg];
		} else {
			colfg.red = ~fg->color.red;
			colfg.green = ~fg->color.green;
			colfg.blue = ~fg->color.blue;
			colfg.alpha = fg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg,
					&revfg);
			fg = &revfg;
		}

		if (bg == &dc.col[defaultbg]) {
			bg = &dc.col[defaultfg];
		} else {
			colbg.red = ~bg->color.red;
			colbg.green = ~bg->color.green;
			colbg.blue = ~bg->color.blue;
			colbg.alpha = bg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg,
					&revbg);
			bg = &revbg;
		}
	}

	/* Highlight current line in vim nav mode (outside prompt space).
	 * Only apply to glyphs with the default background - preserves custom
	 * backgrounds set by programs (fastfetch, etc.) and cursor colors. */
	if (y == vimnav_curline_y() && base.bg == defaultbg)
		bg = &dc.col[vimnav_curline_bg];

	/* Debug mode: highlight prompt lines with yellow tinge */
	if (debug_mode && base.bg == defaultbg) {
		int ps, pe;
		vimnav_prompt_line_range(&ps, &pe);
		if (ps >= 0 && y >= ps && y <= pe)
			bg = &dc.col[debug_prompt_bg];
	}

	if (base.mode & ATTR_SELECTED)
		bg = &dc.col[selectionbg];

	if (base.mode & ATTR_MATCH) {
		fg = bg;
		bg = &dc.col[search_match_bg];
	}

	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
		colfg.red = fg->color.red / 2;
		colfg.green = fg->color.green / 2;
		colfg.blue = fg->color.blue / 2;
		colfg.alpha = fg->color.alpha;
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &revfg);
		fg = &revfg;
	}

	if (base.mode & ATTR_REVERSE) {
		temp = fg;
		fg = bg;
		bg = temp;
	}

	if (base.mode & ATTR_BLINK && win.mode & MODE_BLINK)
		fg = bg;

	if (base.mode & ATTR_INVISIBLE)
		fg = bg;

	if (dmode & DRAW_BG) {
		/* Intelligent cleaning up of the borders. */
		if (x == 0) {
			xclear(0, (y == 0)? 0 : winy, borderpx,
				winy + win.ch +
				((winy + win.ch >= borderpx + win.th)? win.h : 0));
		}
		if (winx + width >= borderpx + win.tw) {
			xclear(winx + width, (y == 0)? 0 : winy, win.w,
				((winy + win.ch >= borderpx + win.th)? win.h : (winy + win.ch)));
		}
		if (y == 0)
			xclear(winx, 0, winx + width, borderpx);
		if (winy + win.ch >= borderpx + win.th)
			xclear(winx, winy + win.ch, winx + width, win.h);
		/* Fill the background */
		XftDrawRect(xw.draw, bg, winx, winy, width, win.ch);
	}

	if (dmode & DRAW_FG) {
		/* Render the glyphs. */
		if (isfilledblock(base.u)) {
			int blocky = winy, blockh = win.ch;
			if (base.u == 0x2580)
				blockh = (win.ch + 1) / 2;
			else if (base.u == 0x2584) {
				blocky += win.ch / 2;
				blockh -= win.ch / 2;
			}
			XftDrawRect(xw.draw, fg, winx, blocky, width, blockh);
		} else {
			XftDrawGlyphFontSpec(xw.draw, fg, specs, len);
		}

		/* Render underline and strikethrough. */
		if (base.mode & ATTR_UNDERLINE) {
			XftDrawRect(xw.draw, fg, winx, winy + dc.font.ascent * chscale + 1,
					width, 1);
		}

		if (base.mode & ATTR_STRUCK) {
			XftDrawRect(xw.draw, fg, winx, winy + 2 * dc.font.ascent * chscale / 3,
					width, 1);
		}
	}
}

void
xdrawglyph(Glyph g, int x, int y)
{
	int numspecs;
	XftGlyphFontSpec spec;

	if (gpu.active) {
		gpudrawcell(g, x, y, 1, 1);
		return;
	}

	numspecs = xmakeglyphfontspecs(&spec, &g, 1, x, y);
	xdrawglyphfontspecs(&spec, g, numspecs, x, y, DRAW_BG | DRAW_FG);
}

void
xdrawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og)
{
	Color drawcol;

	if (gpu.active) {
		gpudrawcursor(cx, cy, g, ox, oy, og);
		return;
	}

	/* remove the old cursor */
	if (selected(ox, oy))
		og.mode |= ATTR_SELECTED;
	if (search_matched(ox, oy))
		og.mode |= ATTR_MATCH;
	xdrawglyph(og, ox, oy);

	if ((IS_SET(MODE_HIDE) && !vimnav.forced) || cmdline_active())
		return;

	/*
	 * Select the right color for the right mode.
	 */
	g.mode &= ATTR_BOLD|ATTR_ITALIC|ATTR_UNDERLINE|ATTR_STRUCK|ATTR_WIDE;

	if (vimnav.forced) {
		/* Coral red cursor for forced nav mode */
		static int allocated = 0;
		static Color forcedcol;
		if (!allocated) {
			XRenderColor rc = { .red = 0xffff, .green = 0x6b6b,
			                    .blue = 0x6b6b, .alpha = 0xffff };
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap,
			                   &rc, &forcedcol);
			allocated = 1;
		}
		drawcol = forcedcol;
		g.fg = defaultbg;
		g.bg = TRUECOLOR(0xff, 0x6b, 0x6b);
	} else if (IS_SET(MODE_REVERSE)) {
		g.mode |= ATTR_REVERSE;
		g.bg = defaultfg;
		if (selected(cx, cy)) {
			drawcol = dc.col[defaultcs];
			g.fg = defaultrcs;
		} else {
			drawcol = dc.col[defaultrcs];
			g.fg = defaultcs;
		}
	} else {
		/* Always use defaultcs for cursor color, even when selected */
		g.fg = defaultbg;
		g.bg = defaultcs;
		drawcol = dc.col[g.bg];
	}

	/* draw the new one */
	if (IS_SET(MODE_FOCUSED)) {
		switch (win.cursor) {
		case 7: /* st extension */
			g.u = 0x2603; /* snowman (U+2603) */
			/* FALLTHROUGH */
		case 0: /* Blinking Block */
		case 1: /* Blinking Block (Default) */
		case 2: /* Steady Block */
			xdrawglyph(g, cx, cy);
			break;
		case 3: /* Blinking Underline */
		case 4: /* Steady Underline */
			XftDrawRect(xw.draw, &drawcol,
					borderpx + cx * win.cw,
					borderpx + (cy + 1) * win.ch - \
						cursorthickness,
					win.cw, cursorthickness);
			break;
		case 5: /* Blinking bar */
		case 6: /* Steady bar */
			XftDrawRect(xw.draw, &drawcol,
					borderpx + cx * win.cw,
					borderpx + cy * win.ch,
					cursorthickness, win.ch);
			break;
		}
	} else {
		XftDrawRect(xw.draw, &drawcol,
				borderpx + cx * win.cw,
				borderpx + cy * win.ch,
				win.cw - 1, 1);
		XftDrawRect(xw.draw, &drawcol,
				borderpx + cx * win.cw,
				borderpx + cy * win.ch,
				1, win.ch - 1);
		XftDrawRect(xw.draw, &drawcol,
				borderpx + (cx + 1) * win.cw - 1,
				borderpx + cy * win.ch,
				1, win.ch - 1);
		XftDrawRect(xw.draw, &drawcol,
				borderpx + cx * win.cw,
				borderpx + (cy + 1) * win.ch - 1,
				win.cw, 1);
	}
}

void
xsetenv(void)
{
	char buf[sizeof(long) * 8 + 1];

	snprintf(buf, sizeof(buf), "%lu", xw.win);
	setenv("WINDOWID", buf, 1);
}

void
xseticontitle(char *p)
{
	XTextProperty prop;
	DEFAULT(p, opt_title);

	if (p[0] == '\0')
		p = opt_title;

	if (Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
	                                &prop) != Success)
		return;
	XSetWMIconName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmiconname);
	XFree(prop.value);
}

void
xsettitle(char *p)
{
	XTextProperty prop;
	DEFAULT(p, opt_title);

	if (p[0] == '\0')
		p = opt_title;

	if (Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
	                                &prop) != Success)
		return;
	XSetWMName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmname);
	XFree(prop.value);
}

void
xsetcwd(char *cwd)
{
	XChangeProperty(xw.dpy, xw.win, xw.stcwd,
			XInternAtom(xw.dpy, "UTF8_STRING", False), 8,
			PropModeReplace, (uchar *)cwd, strlen(cwd));
}

void
xsetdwmsaveargv(const char *argv)
{
	Atom prop = XInternAtom(xw.dpy, "_DWM_SAVE_ARGV", False);
	Atom utf8 = XInternAtom(xw.dpy, "UTF8_STRING", False);

	XChangeProperty(xw.dpy, xw.win, prop, utf8, 8,
			PropModeReplace, (const uchar *)argv, strlen(argv));
}

int
xstartdraw(void)
{
	float bg[3];
	int i, y;

	if (IS_SET(MODE_SYNC))
		return 0;
	if (gpudraw && !gpu.active) {
		gpuinit();
		if (gpu.active)
			xhints();
	}
	if (gpu.active) {
		if (!IS_SET(MODE_VISIBLE) || !IS_SET(MODE_ONSCREEN)) {
			graphics_compact_images();
			return 0;
		}
		glXMakeCurrent(xw.dpy, xw.win, gpu.ctx);
		if (++gpu.frame == 0) {
			GpuImageTexture *image;
			gpu.frame = 1;
			for (image = gpu.images; image; image = image->next)
				image->frame = 0;
		}
		gpuresize();
		gpudamageensure();
		if (gpu.doublebuf && gpu.damage[0]) {
			gpu.damageidx = (gpu.damageidx + 1) % GPU_DAMAGE_HISTORY;
			memset(gpu.damage[gpu.damageidx], 0,
			       gpu.damagerows * sizeof(*gpu.damage[gpu.damageidx]));
		}
		gpubatchreset();
		gpu.backage = 0;
		if (gpu.doublebuf && gpu.bufferage)
			glXQueryDrawable(xw.dpy, xw.win, GLX_BACK_BUFFER_AGE_EXT, &gpu.backage);
		gpu.clearedframe = 0;
		if (gpu.needclear || (gpu.doublebuf && (!gpu.bufferage || gpu.backage == 0 || gpu.backage > GPU_DAMAGE_HISTORY))) {
			gpucolor(IS_SET(MODE_REVERSE) ? defaultfg : defaultbg, bg);
			if (!gpu.clearvalid || !gpucoloreq(gpu.clearcolor, bg)) {
				memcpy(gpu.clearcolor, bg, sizeof gpu.clearcolor);
				gpu.clearvalid = 1;
				glClearColor(bg[0], bg[1], bg[2], 1.0f);
			}
			glClear(GL_COLOR_BUFFER_BIT);
			gpu.clearedframe = 1;
			/*
			 * With double-buffered GLX drawables the back buffer's contents after
			 * a swap are undefined (often black/stale).  The optimized GPU path
			 * normally redraws only dirty rows, which is correct for a preserved
			 * front buffer but leaves untouched rows black after a swap.  Rebuild a
			 * complete frame whenever we are presenting via swaps.
			 */
			tfulldirt();
			gpu.needclear = 0;
		} else if (gpu.doublebuf && gpu.backage > 1 && gpu.damage[0]) {
			for (i = 1; i < (int)gpu.backage; i++) {
				int di = (gpu.damageidx - i + GPU_DAMAGE_HISTORY) % GPU_DAMAGE_HISTORY;
				for (y = 0; y < gpu.damagerows; y++)
					if (gpu.damage[di][y])
						tsetdirt(y, y);
			}
		}
		/* Image quads are emitted every frame in z-order.  Rebuild the
		 * complete text grid while any are visible so a negative-z image
		 * never composites over text retained from an older back buffer. */
		if (graphics_placement_count()) {
			tlineviewprepare();
			if (graphics_has_visible_placements(tisaltscreen(), trow(),
			    tlineviewrow))
				tfulldirt();
		}
		return 1;
	}
	xensurexftbuf(win.cw ? MAX(1, win.tw / win.cw) : 1);
	return IS_SET(MODE_VISIBLE) && IS_SET(MODE_ONSCREEN);
}

void
xdrawline(Line line, int x1, int y1, int x2)
{
	int i, x, ox, numspecs, numspecs_cached;
	Glyph base, new;
	XftGlyphFontSpec *specs;

	if (gpu.active) {
		gpudrawline(line, x1, y1, x2);
		return;
	}

	numspecs_cached = xmakeglyphfontspecs(xw.specbuf, &line[x1], x2 - x1, x1, y1);

	/* Draw line in 2 passes: background and foreground. This way wide glyphs
	   won't get truncated (#223) */
	for (int dmode = DRAW_BG; dmode <= DRAW_FG; dmode <<= 1) {
		specs = xw.specbuf;
		numspecs = numspecs_cached;
		i = ox = 0;
		for (x = x1; x < x2 && i < numspecs; x++) {
			new = line[x];
			if (new.mode == ATTR_WDUMMY)
				continue;
			if (selected(x, y1))
				new.mode |= ATTR_SELECTED;
			if (search_matched(x, y1))
				new.mode |= ATTR_MATCH;
			if (i > 0 && (ATTRCMP(base, new) ||
			    isfilledblock(base.u) || isfilledblock(new.u))) {
				xdrawglyphfontspecs(specs, base, i, ox, y1, dmode);
				specs += i;
				numspecs -= i;
				i = 0;
			}
			if (i == 0) {
				ox = x;
				base = new;
			}
			i++;
		}
		if (i > 0)
			xdrawglyphfontspecs(specs, base, i, ox, y1, dmode);
	}

	/* Debug mode: draw "prompt line" hint text after content */
	if (debug_mode) {
		int ps, pe;
		vimnav_prompt_line_range(&ps, &pe);
		if (ps >= 0 && y1 >= ps && y1 <= pe) {
			const char *label = "     prompt line";
			int label_len = 16;
			int ll = tlinelen(y1);
			int text_x = borderpx + (ll > 0 ? ll : 0) * win.cw;
			int text_y = borderpx + y1 * win.ch + dc.font.ascent;
			int label_width = label_len * win.cw;
			/* Only draw if it fits on screen */
			if (text_x + label_width <= borderpx + win.tw) {
				XftDrawRect(xw.draw, &dc.col[debug_prompt_bg],
						text_x, borderpx + y1 * win.ch,
						label_width, win.ch);
				XftDrawStringUtf8(xw.draw, &dc.col[debug_prompt_fg],
						dc.font.match, text_x, text_y,
						(const FcChar8 *)label, label_len);
			}
		}
	}
}

void
xfinishdraw(void)
{
	if (gpu.active) {
		gpudrawimages(GRAPHICS_STAGE_BELOW_BACKGROUND);
		gpudrawbatch(&gpu.bg, 0);
		gpudrawimages(GRAPHICS_STAGE_BELOW_TEXT);
		gpudrawbatch(&gpu.text, 1);
		gpudrawbatch(&gpu.ctext, 2);
		gpudrawbatch(&gpu.deco, 0);
		gpudrawimages(GRAPHICS_STAGE_ABOVE_TEXT);
		gpudrawbatch(&gpu.obg, 0);
		gpudrawbatch(&gpu.otext, 1);
		gpudrawbatch(&gpu.octext, 2);
		gpudrawbatch(&gpu.odeco, 0);
		gpupruneimages();
		graphics_compact_images();
		glDisableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_COLOR_ARRAY);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glDisable(GL_TEXTURE_2D);
		if (gpu.doublebuf)
			glXSwapBuffers(xw.dpy, xw.win);
		else
			glFlush();
		return;
	}
	XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, 0, 0, win.w,
			win.h, 0, 0);
	XSetForeground(xw.dpy, dc.gc,
			dc.col[IS_SET(MODE_REVERSE)?
				defaultfg : defaultbg].pixel);
}

void
xximspot(int x, int y)
{
	if (xw.ime.xic == NULL)
		return;

	xw.ime.spot.x = borderpx + x * win.cw;
	xw.ime.spot.y = borderpx + (y + 1) * win.ch;
	if (gpu.active) {
		xw.ime.spot.x = borderpx + x * gpucellw();
		xw.ime.spot.y = borderpx + (y + 1) * gpucellh();
	}

	XSetICValues(xw.ime.xic, XNPreeditAttributes, xw.ime.spotlist, NULL);
}

void
expose(XEvent *ev)
{
	redraw();
	sshind_draw();
	notif_draw();
	cmdline_draw();
}

void
visibility(XEvent *ev)
{
	XVisibilityEvent *e = &ev->xvisibility;

	MODBIT(win.mode, e->state != VisibilityFullyObscured, MODE_VISIBLE);
	if (e->state == VisibilityFullyObscured && gpu.active)
		gpureleaseimages();
}

void
unmap(XEvent *ev)
{
	win.mode &= ~MODE_VISIBLE;
	if (gpu.active)
		gpureleaseimages();
}

void
xsetpointermotion(int set)
{
	MODBIT(xw.attrs.event_mask, set, PointerMotionMask);
	XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
}

void
xsetmode(int set, unsigned int flags)
{
	int mode = win.mode;
	MODBIT(win.mode, set, flags);
	if (!set && (flags & MODE_PASTEEVENT))
		clip5522_token.valid = 0;
	if ((win.mode & MODE_REVERSE) != (mode & MODE_REVERSE))
		redraw();
	if ((win.mode & MODE_MOUSE) != (mode & MODE_MOUSE)) {
		xinitinputcursor();
		XDefineCursor(xw.dpy, xw.win, IS_SET(MODE_MOUSE)
			? xcursorpointer : xcursortext);
	}
	if ((win.mode ^ mode) & MODE_SYNC) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		syncupdate_set(&syncUpdate, set, &now);
	}
}

int
xismode(unsigned int flags)
{
	return (win.mode & flags) == flags;
}

void
xresetmode(void)
{
	xsetpointermotion(0);
	xsetmode(0, win.mode ^ winmoderestore(win.mode));
}

void
xsetmousecursor(int shape)
{
	Cursor c;
	xinitinputcursor();
	switch (shape) {
	case 1:  c = xcursorpointer; break;
	case 2:  c = xcursorhand;    break;
	default: c = xcursortext;    break;
	}
	XDefineCursor(xw.dpy, xw.win, c);
}

int
xgetcursor(void)
{
	return win.cursor;
}

int
xsetcursor(int cursor)
{
	if (!BETWEEN(cursor, 0, 7)) /* 7: st extension */
		return 1;
	win.cursor = cursor;
	return 0;
}

void
xseturgency(int add)
{
	XWMHints *h = XGetWMHints(xw.dpy, xw.win);
	if (!h) {
		h = XAllocWMHints();
		if (!h)
			return;
		h->flags = 0;
	}

	MODBIT(h->flags, add, XUrgencyHint);
	XSetWMHints(xw.dpy, xw.win, h);
	XFree(h);
}

void
xbell(void)
{
	if (!(IS_SET(MODE_FOCUSED)))
		xseturgency(1);
	if (bellvolume)
		XkbBell(xw.dpy, xw.win, bellvolume, (Atom)NULL);
}

void
xcleanup(void)
{
	gpudestroy();
}

void
focus(XEvent *ev)
{
	XFocusChangeEvent *e = &ev->xfocus;

	if (e->mode == NotifyGrab)
		return;

	if (ev->type == FocusIn) {
		if (xw.ime.xic)
			XSetICFocus(xw.ime.xic);
		win.mode |= MODE_FOCUSED;
		xseturgency(0);
		if (IS_SET(MODE_FOCUS))
			ttywrite("\033[I", 3, 0);
	} else {
		if (xw.ime.xic)
			XUnsetICFocus(xw.ime.xic);
		win.mode &= ~MODE_FOCUSED;
		if (IS_SET(MODE_FOCUS))
			ttywrite("\033[O", 3, 0);
	}
}

int
match(uint mask, uint state)
{
	return mask == XK_ANY_MOD || mask == (state & ~ignoremod);
}

char*
kmap(KeySym k, uint state)
{
	Key *kp;
	int i;

	/* Check for mapped keys out of X11 function keys. */
	for (i = 0; i < LEN(mappedkeys); i++) {
		if (mappedkeys[i] == k)
			break;
	}
	if (i == LEN(mappedkeys)) {
		if ((k & 0xFFFF) < 0xFD00)
			return NULL;
	}

	for (kp = key; kp < key + LEN(key); kp++) {
		if (kp->k != k)
			continue;

		if (!match(kp->mask, state))
			continue;

		if (IS_SET(MODE_APPKEYPAD) ? kp->appkey < 0 : kp->appkey > 0)
			continue;
		if (IS_SET(MODE_NUMLOCK) && kp->appkey == 2)
			continue;

		if (IS_SET(MODE_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
			continue;

		return kp->s;
	}

	return NULL;
}

static int
kittykbd_modifier(uint state)
{
	int mod = 1;
	if (state & ShiftMask)
		mod += 1;
	if (state & Mod1Mask)
		mod += 2;
	if (state & ControlMask)
		mod += 4;
	if (state & Mod4Mask)
		mod += 8;
	return mod;
}

static long
kittykbd_decode_utf8(const char *buf, int len)
{
	const unsigned char *s = (const unsigned char *)buf;
	if (len <= 0)
		return 0;
	if (s[0] < 0x80)
		return s[0];
	if (len >= 2 && (s[0] & 0xe0) == 0xc0)
		return ((s[0] & 0x1f) << 6) | (s[1] & 0x3f);
	if (len >= 3 && (s[0] & 0xf0) == 0xe0)
		return ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
	if (len >= 4 && (s[0] & 0xf8) == 0xf0)
		return ((s[0] & 0x07) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
	return 0;
}

static long
kittykbd_keycode(KeySym ksym, uint state, const char *buf, int len)
{
	if ((state & ControlMask) && ksym >= XK_A && ksym <= XK_Z)
		return 'a' + (ksym - XK_A);
	if ((state & ControlMask) && ksym >= XK_a && ksym <= XK_z)
		return ksym;

	long decoded = kittykbd_decode_utf8(buf, len);
	if (decoded)
		return decoded;

	switch (ksym) {
	case XK_Return:    return 13;
	case XK_Tab:       return 9;
	case XK_BackSpace: return 127;
	case XK_Escape:    return 27;
	case XK_space:     return 32;
	default:
		if ((ksym & 0xff000000) == 0x01000000)
			return (long)(ksym & 0x00ffffff);
		if (ksym >= 0x20 && ksym <= 0xff)
			return (long)ksym;
		return 0;
	}
}

static int
kittykbd_write(KeySym ksym, uint state, const char *buf, int len, int event)
{
	char seq[128];
	long code = kittykbd_keycode(ksym, state, buf, len);
	if (!code)
		return 0;
	int n = snprintf(seq, sizeof(seq), "\033[%ld;%d:%du", code, kittykbd_modifier(state), event);
	if (n > 0 && n < (int)sizeof(seq)) {
		ttywrite(seq, n, 1);
		return 1;
	}
	return 0;
}

int
xisautorepeatrelease(XEvent *ev)
{
	XEvent next;

	if (ev->type != KeyRelease)
		return 0;

	/* Without XKB detectable auto-repeat, X11 represents a repeated key as
	 * KeyRelease immediately followed by KeyPress for the same key and time.
	 * Do not forward that synthetic release to applications using kitty's
	 * keyboard event-type protocol: held Space must remain "pressed" until the
	 * actual physical release. */
	if (XEventsQueued(xw.dpy, QueuedAfterReading) <= 0)
		return 0;

	XPeekEvent(xw.dpy, &next);
	return next.type == KeyPress
		&& next.xkey.keycode == ev->xkey.keycode
		&& next.xkey.time == ev->xkey.time;
}

void
kpress(XEvent *ev)
{
	XKeyEvent *e = &ev->xkey;
	KeySym ksym = NoSymbol;
	char buf[64], *customkey;
	int len;
	Rune c;
	Status status;
	Shortcut *bp;

	if (IS_SET(MODE_KBDLOCK))
		return;

	if (xw.ime.xic) {
		len = XmbLookupString(xw.ime.xic, e, buf, sizeof buf, &ksym, &status);
		if (status == XBufferOverflow)
			return;
	} else {
		len = XLookupString(e, buf, sizeof buf, &ksym, NULL);
	}

	/* Command-line mode: intercept all keys */
	if (cmdline_active()) {
		cmdline_handle_key(ksym, e->state, buf, len);
		return;
	}

	if (ksym == XK_Shift_R && rightshiftseq && *rightshiftseq) {
		uint masked = e->state & ~(LockMask | Mod2Mask);
		if (masked == 0) {
			ttywrite(rightshiftseq, strlen(rightshiftseq), 1);
			return;
		}
	}

	/* Shift+Escape: force-toggle vim nav mode */
	if (ksym == XK_Escape && (e->state & ShiftMask)) {
		if (vimnav.forced) {
			vimnav_exit();
		} else if (!tisvimnav()) {
			vimnav_force_enter();
		} else {
			/* Already in regular nav mode: upgrade to forced */
			vimnav.forced = 1;
			tfulldirt();
		}
		return;
	}

	/* Vim navigation mode key handling */
	if (tisvimnav()) {
		if (vimnav_handle_key(ksym, e->state))
			return;
		/* Unknown key: pass through to shell but stay in vim nav mode.
		 * Zsh will send vim-mode;exit when it leaves vicmd mode. */
	}

	/* Ctrl+number row: type special chars in shell, F-key seqs in alt screen */
	if (!tisaltscreen() && (e->state & ControlMask)) {
		const char *insert = NULL;
		switch (ksym) {
		case XK_1: insert = "\xe2\x86\x90"; break; /* ← (F14) */
		case XK_2: insert = "\xe2\x80\xa2"; break; /* • (F15) */
		case XK_3: insert = "\xe2\x86\x92"; break; /* → (F16) */
		case XK_4: insert = "<F17>"; break;
		case XK_5: insert = "<F18>"; break;
		case XK_6: insert = "<F19>"; break;
		case XK_7: insert = "<F20>"; break;
		case XK_8: insert = "<F21>"; break;
		case XK_9: insert = "\xe2\x80\xa6"; break; /* … (F22) */
		case XK_0: insert = "\xe2\x80\x93"; break; /* – (F23) */
		case XK_minus: insert = "\xe2\x80\x94"; break; /* — (F24) */
		}
		if (insert) {
			ttywrite(insert, strlen(insert), 1);
			return;
		}
	}

	/* Keep ordinary ^V for shells, except while a TUI explicitly opted into
	 * DEC 5522 paste events.  Ctrl+Shift+V remains the configured text-paste
	 * shortcut (which itself uses the rich event while that mode is enabled). */
	if (IS_SET(MODE_PASTEEVENT) && (ksym == XK_v || ksym == XK_V) &&
		(e->state & (ControlMask | ShiftMask)) == ControlMask &&
		!(e->state & ~(ControlMask | LockMask | Mod2Mask))) {
		x5522paste(0);
		return;
	}

	/* 1. shortcuts */
	for (bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if (ksym == bp->keysym && match(bp->mod, e->state)) {
			bp->func(&(bp->arg));
			return;
		}
	}

	/* 2. custom keys from config.h */
	if ((customkey = kmap(ksym, e->state))) {
		ttywrite(customkey, strlen(customkey), 1);
		return;
	}

	if (IS_SET(MODE_KITTYKBD) && kittykbd_write(ksym, e->state, buf, len, 1))
		return;

	/* 3. composed string from input method */
	if (len == 0)
		return;
	if (len == 1 && e->state & Mod1Mask) {
		if (IS_SET(MODE_8BIT)) {
			if (*buf < 0177) {
				c = *buf | 0x80;
				len = utf8encode(c, buf);
			}
		} else {
			buf[1] = buf[0];
			buf[0] = '\033';
			len = 2;
		}
	}
	if (!tisaltscreen()) {
		char modbuf[64] = "";
		if (e->state & ControlMask)
			strcat(modbuf, "Ctrl+");
		if (e->state & ShiftMask)
			strcat(modbuf, "Shift+");
		if (e->state & Mod1Mask)
			strcat(modbuf, "Alt+");
		if (e->state & Mod4Mask)
			strcat(modbuf, "Super+");

		char *key = XKeysymToString(ksym);
		char fullkey[128] = "";
		strcat(fullkey, modbuf);
		strcat(fullkey, key);
		/* printf("Key: %s\n",fullkey); */

		if (strcmp(fullkey, "Ctrl+e") == 0) {
			kscrolldown(&(Arg){ .i = 1 });
			return;
		}

		if (strcmp(fullkey, "Ctrl+y") == 0) {
			kscrollup(&(Arg){ .i = 1 });
			return;
		}

		if (strcmp(fullkey, "Ctrl+u") == 0) { /* Hardcoded but I can't be bothered. */
			kscrollup(&(Arg){ .i = 52/2 });
			return;
		}

		if (strcmp(fullkey, "Ctrl+d") == 0) {
			kscrolldown(&(Arg){ .i = 52/2 });
			return;
		}

		if (strcmp(fullkey, "Ctrl+b") == 0) { /* Hardcoded but I can't be bothered. */
			kscrollup(&(Arg){ .i = 52 });
			return;
		}

		if (strcmp(fullkey, "Ctrl+f") == 0) {
			kscrolldown(&(Arg){ .i = 52 });
			return;
		}
	}

	ttywrite(buf, len, 1);
}

void
krelease(XEvent *ev)
{
	XKeyEvent *e = &ev->xkey;
	KeySym ksym = NoSymbol;
	char buf[64];
	int len;
	Status status;

	if (IS_SET(MODE_KBDLOCK) || !IS_SET(MODE_KITTYKBD))
		return;

	if (xw.ime.xic) {
		len = XmbLookupString(xw.ime.xic, e, buf, sizeof buf, &ksym, &status);
		if (status == XBufferOverflow)
			return;
	} else {
		len = XLookupString(e, buf, sizeof buf, &ksym, NULL);
	}
	if (ksym == NoSymbol)
		ksym = XLookupKeysym(e, 0);

	kittykbd_write(ksym, e->state, buf, len, 3);
}

void
cmessage(XEvent *e)
{
	/*
	 * See xembed specs
	 *  http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html
	 */
	if (e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
		if (e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
			win.mode |= MODE_FOCUSED;
			xseturgency(0);
		} else if (e->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
			win.mode &= ~MODE_FOCUSED;
		}
	} else if (e->xclient.data.l[0] == xw.wmdeletewin) {
		gpudestroy();
		ttyhangup();
		persist_save();
		persist_cleanup();
		_exit(0);
	}
}

void
resize(XEvent *e)
{
	int onscreen, wasonscreen;

	/* dwm hides clients by moving them completely outside the root window.
	 * VisibilityNotify does not reliably classify such mapped windows as
	 * obscured, so without this check a hidden terminal continues submitting
	 * GL frames.  Keep this bit separate from MODE_VISIBLE: an on-screen
	 * terminal can still be fully obscured by another client. */
	wasonscreen = IS_SET(MODE_ONSCREEN);
	onscreen = e->xconfigure.x < DisplayWidth(xw.dpy, xw.scr) &&
		e->xconfigure.y < DisplayHeight(xw.dpy, xw.scr) &&
		e->xconfigure.x + e->xconfigure.width > 0 &&
		e->xconfigure.y + e->xconfigure.height > 0;
	MODBIT(win.mode, onscreen, MODE_ONSCREEN);
	if (onscreen && !wasonscreen)
		tfulldirt();
	else if (!onscreen && wasonscreen && gpu.active)
		gpureleaseimages();

	if (e->xconfigure.width == win.w && e->xconfigure.height == win.h)
		return;

	cresize(e->xconfigure.width, e->xconfigure.height);
	sshind_resize();
	notif_resize();
	cmdline_resize();
}

static void
sigterm(int sig)
{
	(void)sig;
	persist_save();
	_exit(0);
}

void
run(void)
{
	XEvent ev;
	int w = win.w, h = win.h;
	fd_set rfd;
	int xfd = XConnectionNumber(xw.dpy), ttyfd, xev, drawing;
	int selret, ttyready, redraw;
	struct timespec seltv, *tv, now, lastblink, trigger;
	double timeout;

	/* Waiting for window mapping */
	do {
		XNextEvent(xw.dpy, &ev);
		/*
		 * This XFilterEvent call is required because of XOpenIM. It
		 * does filter out the key event and some client message for
		 * the input method too.
		 */
		if (XFilterEvent(&ev, None))
			continue;
		if (ev.type == ConfigureNotify) {
			w = ev.xconfigure.width;
			h = ev.xconfigure.height;
		}
	} while (ev.type != MapNotify);
	if (gpudraw) {
		Bool detectable_autorepeat_supported;
		XkbSetDetectableAutoRepeat(xw.dpy, True, &detectable_autorepeat_supported);
	}

	ttyfd = ttynew(opt_line, shell, opt_io, opt_cmd);
	cresize(w, h);

	/* Initialize command-line child window (needs geometry from cresize) */
	cmdline_init();

	/* Re-execute an altscreen command only when restoring a saved terminal.
	 * Initial -e launches also capture altcmd, but that command is already
	 * running; writing it to the pty here would feed it into the application. */
	if (persist_should_reexecute_altcmd(opt_fromsave != NULL)) {
		if (debug_mode)
			fprintf(stderr, "[persist] re-executing altcmd: %s\n",
					persist_get_altcmd());
		ttywrite(persist_get_altcmd(),
				strlen(persist_get_altcmd()), 1);
		ttywrite("\n", 1, 1);
	} else if (opt_fromorphan && !opt_fromsave) {
		static const char msg[] =
			"echo 'st: no orphan save directory found'\n";
		ttywrite(msg, sizeof(msg) - 1, 1);
	}

	struct timespec lastpersist = {0};
	for (timeout = -1, drawing = 0, lastblink = (struct timespec){0};;) {
		FD_ZERO(&rfd);
		FD_SET(ttyfd, &rfd);
		FD_SET(xfd, &rfd);

		if (XPending(xw.dpy))
			timeout = 0;  /* existing events might not set xfd */

		seltv.tv_sec = timeout / 1E3;
		seltv.tv_nsec = 1E6 * (timeout - 1E3 * seltv.tv_sec);
		tv = timeout >= 0 ? &seltv : NULL;

		if ((selret = pselect(MAX(xfd, ttyfd)+1, &rfd, NULL, NULL, tv, NULL)) < 0) {
			if (errno == EINTR) {
				if (childready())
					reapchild();
				continue;
			}
			die("select failed: %s\n", strerror(errno));
		}

		if (childready())
			reapchild();
		clock_gettime(CLOCK_MONOTONIC, &now);
		clip5522_tick(&now);

		ttyready = FD_ISSET(ttyfd, &rfd);
		if (ttyready)
			ttyread();

		xev = 0;
		while (XPending(xw.dpy)) {
			xev = 1;
			XNextEvent(xw.dpy, &ev);
			if (xisautorepeatrelease(&ev))
				continue;
			/* XIM may filter KeyRelease even though the kitty keyboard
			 * protocol needs release events for event type reporting. */
			if (ev.type != KeyRelease && XFilterEvent(&ev, None))
				continue;
			if (handler[ev.type])
				(handler[ev.type])(&ev);
		}

		/*
		 * To reduce flicker and tearing, when new content or event
		 * triggers drawing, we first wait a bit to ensure we got
		 * everything, and if nothing new arrives - we draw.
		 * We start with trying to wait minlatency ms. If more content
		 * arrives sooner, we retry with shorter and shorter periods,
		 * and eventually draw even without idle after maxlatency ms.
		 * Typically this results in low latency while interacting,
		 * maximum latency intervals during `cat huge.txt`, and perfect
		 * sync with periodic updates from animations/key-repeats/etc.
		 */
		if (ttyready || xev) {
			if (!drawing) {
				trigger = now;
				drawing = 1;
			}
			timeout = (maxlatency - TIMEDIFF(now, trigger)) \
			          / maxlatency * minlatency;
			if (timeout > 0)
				continue;  /* we have time, try to find idle */
		}

		/* The X fd can become readable due to internal GLX/DRI traffic without
		 * yielding any X events.  Do not let that cut short the draw latency timer
		 * or create an idle redraw/swap loop. */
		if (drawing && !ttyready && !xev && selret > 0) {
			timeout = (maxlatency - TIMEDIFF(now, trigger)) \
			          / maxlatency * minlatency;
			if (timeout > 0)
				continue;
		}

		/* DEC mode 2026 mutates the live grid normally but withholds all
		 * presentation until ESU.  A finite watchdog recovers from a missing
		 * reset sequence instead of leaving the window frozen forever. */
		if (IS_SET(MODE_SYNC)) {
			timeout = syncupdate_remaining(&syncUpdate, &now, synctimeout);
			if (timeout > 0)
				continue;
			xsetmode(0, MODE_SYNC);
		}

		/* idle detected or maxlatency exhausted -> draw */
		redraw = drawing;
		timeout = -1;
		if (blinktimeout && tattrset(ATTR_BLINK)) {
			timeout = blinktimeout - TIMEDIFF(now, lastblink);
			if (timeout <= 0) {
				if (-timeout > blinktimeout) /* start visible */
					win.mode |= MODE_BLINK;
				win.mode ^= MODE_BLINK;
				tsetdirtattr(ATTR_BLINK);
				lastblink = now;
				timeout = blinktimeout;
				redraw = 1;
			}
		}

		if (notif_active()) {
			int notif_remain = notif_check_timeout(&now);
			if (selret == 0)
				redraw = 1;
			if (notif_remain > 0) {
				if (timeout < 0 || notif_remain < timeout)
					timeout = notif_remain;
			}
		}

		if (persist_active()) {
			double persist_remain = persistinterval
					- TIMEDIFF(now, lastpersist);
			if (persist_remain <= 0) {
				persist_save();
				lastpersist = now;
				persist_remain = persistinterval;
			}
			if (timeout < 0 || persist_remain < timeout)
				timeout = persist_remain;
		}

		if (!redraw)
			continue;
		draw();
		XFlush(xw.dpy);
		drawing = 0;
	}
}

void
usage(void)
{
	die("usage: %s [-adiv] [-c class] [-f font] [-g geometry]"
	    " [-n name] [-o file]\n"
	    "          [-T title] [-t title] [-w windowid]"
	    " [--from-save dir]"
	    " [[-e] command [args ...]]\n"
	    "       %s [-adiv] [-c class] [-f font] [-g geometry]"
	    " [-n name] [-o file]\n"
	    "          [-T title] [-t title] [-w windowid] -l line"
	    " [stty_args ...]\n", argv0, argv0);
}

int
main(int argc, char *argv[])
{
	xw.l = xw.t = 0;
	xw.isfixed = False;
	xsetcursor(cursorshape);

	/* Parse long opts before ARGBEGIN (arg.h doesn't support them) */
	{
		int i;
		for (i = 1; i < argc; i++) {
			if (!strcmp("--from-save", argv[i]) && i + 1 < argc) {
				opt_fromsave = argv[i + 1];
				memmove(&argv[i], &argv[i + 2],
						(argc - i - 2 + 1) * sizeof(char *));
				argc -= 2;
				break;
			}
			if (!strcmp("--from-orphan", argv[i])) {
				opt_fromorphan = 1;
				memmove(&argv[i], &argv[i + 1],
						(argc - i - 1 + 1) * sizeof(char *));
				argc -= 1;
				break;
			}
		}
	}

	ARGBEGIN {
	case 'a':
		allowaltscreen = 0;
		break;
	case 'c':
		opt_class = EARGF(usage());
		break;
	case 'e':
		if (argc > 0)
			--argc, ++argv;
		goto run;
	case 'f':
		opt_font = EARGF(usage());
		break;
	case 'g':
		xw.gm = XParseGeometry(EARGF(usage()),
				&xw.l, &xw.t, &cols, &rows);
		break;
	case 'i':
		xw.isfixed = 1;
		break;
	case 'o':
		opt_io = EARGF(usage());
		break;
	case 'l':
		opt_line = EARGF(usage());
		break;
	case 'n':
		opt_name = EARGF(usage());
		break;
	case 't':
	case 'T':
		opt_title = EARGF(usage());
		break;
	case 'w':
		opt_embed = EARGF(usage());
		break;
	case 'd':
		debug_mode = 1;
		break;
	case 'v':
		die("%s " VERSION "\n", argv0);
		break;
	default:
		usage();
	} ARGEND;

run:
	if (argc > 0) { /* eat all remaining arguments */
		opt_cmd = argv;
		/* A command supplied with -e owns the terminal lifetime.  Preserve that
		 * behavior across restore instead of starting a durable shell and typing
		 * the captured command into it. */
		persist_set_ephemeral(1);

		/*
		 * Save -e command for ephemeral persist restore.
		 * preexec (OSC 780) doesn't fire for zsh -c commands,
		 * so we capture the command from the -e args directly.
		 *
		 * Detect "shell -c cmd" / "shell -ic cmd" pattern and
		 * extract the inner command, since the ephemeral restore
		 * already wraps with "$SHELL -ic <altcmd>".
		 * Otherwise, join all args as a shell command string.
		 */
		{
			int has_cflag = 0;
			if (argc >= 3 && argv[1][0] == '-') {
				const char *p;
				for (p = argv[1] + 1; *p; p++) {
					if (*p == 'c') { has_cflag = 1; break; }
				}
			}
			if (has_cflag) {
				persist_set_altcmd(argv[2]);
			} else {
				char buf[PATH_MAX];
				size_t pos = 0;
				int i;
				for (i = 0; i < argc && pos < sizeof(buf) - 1; i++) {
					size_t len = strlen(argv[i]);
					if (i > 0 && pos < sizeof(buf) - 1)
						buf[pos++] = ' ';
					if (pos + len > sizeof(buf) - 1)
						len = sizeof(buf) - 1 - pos;
					memcpy(buf + pos, argv[i], len);
					pos += len;
				}
				buf[pos] = '\0';
				persist_set_altcmd(buf);
			}
		}
	}

	if (!opt_title)
		opt_title = (opt_line || !opt_cmd) ? "st" : opt_cmd[0];

	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");
	cols = MAX(cols, 1);
	rows = MAX(rows, 1);
	tnew(cols, rows);
	if (opt_fromorphan && !opt_fromsave) {
		const char *orphan = persist_find_orphan();
		if (orphan)
			opt_fromsave = (char *)orphan;
	}
	if (opt_fromsave) {
		persist_restore(opt_fromsave, &cols, &rows);
	}
	xinit(cols, rows);
	xsetenv();
	persist_init(getpid());
	persist_register();
	signal(SIGTERM, sigterm);
	selinit();
	run();

	return 0;
}
