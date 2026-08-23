#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>

#ifdef MIN
#undef MIN
#endif
#ifdef MAX
#undef MAX
#endif

#include <arpa/inet.h>
#include <ctype.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

char *argv0;
#include "../arg.h"
#include "../st.h"
#include "../win.h"
#include "../graphics.h"
#include "../persist.h"
#include "../vimnav.h"
#include "../cmdline.h"
#include "../search.h"
#include "keysyms.h"
#include "native.h"
#include "locale.h"
#include "pty.h"
#include "reveal.h"
#include "pasteboard5522.h"
#include "../sync.h"

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
	uint release;
} MouseShortcut;

typedef struct {
	KeySym k;
	uint mask;
	char *s;
	signed char appkey;
	signed char appcursor;
} Key;

static void clipcopy(const Arg *);
void clippaste(const Arg *);
static void numlock(const Arg *);
static void selpaste(const Arg *);
static void zoom(const Arg *);
static void zoomabs(const Arg *);
static void zoomreset(const Arg *);
static void ttysend(const Arg *);

#include "../config.h"
#include "../sshind.h"
#include "../notif.h"

#define IS_SET(flag) ((win.mode & (flag)) != 0)
#define NOTIF_META_SEP '\x1e'
#define NOTIF_META_DELIM '\x1f'

@class STAppDelegate;

@interface STMetalView : MTKView <MTKViewDelegate, NSTextInputClient>
@property(nonatomic, strong) NSMutableAttributedString *marked;
@property(nonatomic) NSRange markedSelection;
@end

TermWindow win;
char *usedfont;
double usedfontsize;

static NSWindow *nativeWindow;
static STMetalView *nativeView;
static STAppDelegate *appDelegate;
static MacColor *palette;
static size_t paletteCount;
static int ttyfd = -1;
static int drawingFrame;
static int redrawPending;
static SyncUpdate syncUpdate;
static int shuttingDown;
static int cursorKind;
static int imeCol, imeRow;
static uint pressedButtons;
static char *primarySelection;
static NSData *primaryImage;
static STPasteboard5522 *pasteboard5522;
static double defaultFontSize;
static double fontWidthSpacing = 1.0;
static int windowRevealed;
static NSUInteger windowRevealGeneration;
static int managedWindowReveal;
static int managedWindowRevealRequested;
static const char macosDefaultFont[] = "SFMono-Regular:size=11";
static const double macosDefaultFontWidthSpacing = 1.004032258064516;
static dispatch_source_t ttySource;
static dispatch_source_t blinkSource;
static dispatch_source_t persistSource;
static dispatch_source_t notificationSource;
static dispatch_source_t termSource;
static dispatch_source_t interruptSource;
static dispatch_source_t hangupSource;
static dispatch_source_t quitSource;
static dispatch_source_t controlSource;
static dispatch_source_t revealSource;
static int controlFD = -1;
static char controlPath[sizeof(((struct sockaddr_un *)0)->sun_path)];

static char *opt_class;
static char **opt_cmd;
static char *opt_embed;
static char *opt_font;
static char *opt_io;
static char *opt_line;
static char *opt_name;
static char *opt_title;
static char *opt_fromsave;
static int opt_fromorphan;
static int opt_fixed;
static int opt_x, opt_y;
static int opt_has_position;

typedef struct {
	int active;
	char host[256];
	int width, height;
} NativeSSHIndicator;

typedef struct {
	int active;
	char msg[512];
	int lineOffset[NOTIF_MAX_LINES];
	int lineLength[NOTIF_MAX_LINES];
	int lineCount;
	int timeoutMs;
	double fontScale;
	MacColor fg, bg, border;
	struct timespec shown;
	int width, height;
} NativeToast;

static NativeSSHIndicator sshIndicator;
static struct {
	NativeToast toasts[NOTIF_MAX_TOASTS];
	int count;
} notifications;

static void nativeResize(double, double);
static void nativeHandleKey(NSEvent *, int);
static void nativeFocus(int);
static void startControlSocket(void);
static void stopControlSocket(void);
static void cleanupNative(void);
static void buildMenu(void);
static void scheduleWindowReveal(void);
static void updateWindowReveal(void);

static double
parseFontSize(const char *description)
{
	const char *p;
	if (!description)
		return 14.0;
	p = strstr(description, "pixelsize=");
	if (p)
		return MAX(1.0, strtod(p + 10, NULL));
	p = strstr(description, "size=");
	if (p)
		return MAX(1.0, strtod(p + 5, NULL));
	return 14.0;
}

static int
parseHexComponent(const char *value, float *component)
{
	size_t length = strlen(value);
	if (!BETWEEN(length, 1, 4))
		return 0;
	for (size_t i = 0; i < length; i++)
		if (!isxdigit((unsigned char)value[i]))
			return 0;
	unsigned long maximum = (1UL << (4 * length)) - 1;
	*component = strtoul(value, NULL, 16) / (float)maximum;
	return 1;
}

MacColor
macos_parse_color(const char *value, MacColor fallback)
{
	unsigned int red, green, blue, alpha = 255;
	if (!value)
		return fallback;
	if (value[0] == '#') {
		if (strlen(value) == 7 &&
		    sscanf(value + 1, "%02x%02x%02x", &red, &green, &blue) == 3)
			return (MacColor){red / 255.0f, green / 255.0f,
			    blue / 255.0f, 1.0f};
		if (strlen(value) == 9 &&
		    sscanf(value + 1, "%02x%02x%02x%02x", &red, &green,
		    &blue, &alpha) == 4)
				return (MacColor){red / 255.0f, green / 255.0f,
				    blue / 255.0f, alpha / 255.0f};
	}
	if (!strncmp(value, "rgb:", 4) || !strncmp(value, "rgba:", 5)) {
		char rpart[5] = {0}, gpart[5] = {0}, bpart[5] = {0}, apart[5] = {0};
		char extra;
		float rvalue, gvalue, bvalue, avalue = 1.0f;
		int rgba = value[3] == 'a';
		int fields = rgba ?
		    sscanf(value, "rgba:%4[0-9A-Fa-f]/%4[0-9A-Fa-f]/%4[0-9A-Fa-f]/%4[0-9A-Fa-f]%c",
		        rpart, gpart, bpart, apart, &extra) :
		    sscanf(value, "rgb:%4[0-9A-Fa-f]/%4[0-9A-Fa-f]/%4[0-9A-Fa-f]%c",
		        rpart, gpart, bpart, &extra);
		if (fields == (rgba ? 4 : 3) &&
		    parseHexComponent(rpart, &rvalue) &&
		    parseHexComponent(gpart, &gvalue) &&
		    parseHexComponent(bpart, &bvalue) &&
		    (!rgba || parseHexComponent(apart, &avalue)))
			return (MacColor){rvalue, gvalue, bvalue, avalue};
	}
	NSDictionary<NSString *, NSColor *> *named = @{
		@"black": NSColor.blackColor,
		@"white": NSColor.whiteColor,
		@"red": NSColor.redColor,
		@"green": NSColor.greenColor,
		@"blue": NSColor.blueColor,
		@"yellow": NSColor.yellowColor,
		@"cyan": NSColor.cyanColor,
		@"magenta": NSColor.magentaColor,
	};
	NSColor *color = named[[NSString stringWithUTF8String:value].lowercaseString];
	color = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
	if (color)
		return (MacColor){color.redComponent, color.greenComponent,
		    color.blueComponent, color.alphaComponent};
	return fallback;
}

static MacColor
indexedColor(uint32_t color)
{
	if (IS_TRUECOL(color))
		return (MacColor){((color >> 16) & 0xff) / 255.0f,
		    ((color >> 8) & 0xff) / 255.0f,
		    (color & 0xff) / 255.0f, 1.0f};
	if (!paletteCount)
		return (MacColor){1, 1, 1, 1};
	return palette[MIN(color, paletteCount - 1)];
}

void
xloadcols(void)
{
	paletteCount = MAX(LEN(colorname), 256);
	palette = xrealloc(palette, paletteCount * sizeof(*palette));
	for (size_t i = 0; i < paletteCount; i++) {
		if (i < LEN(colorname) && colorname[i]) {
			palette[i] = macos_parse_color(colorname[i],
			    (MacColor){1, 1, 1, 1});
		} else if (BETWEEN(i, 16, 231)) {
			int n = (int)i - 16;
			int rr = n / 36, gg = (n / 6) % 6, bb = n % 6;
			int cv[6] = {0, 95, 135, 175, 215, 255};
			palette[i] = (MacColor){cv[rr] / 255.0f,
			    cv[gg] / 255.0f, cv[bb] / 255.0f, 1};
		} else if (BETWEEN(i, 232, 255)) {
			float gray = (8 + 10 * ((int)i - 232)) / 255.0f;
			palette[i] = (MacColor){gray, gray, gray, 1};
		} else {
			palette[i] = (MacColor){0, 0, 0, 1};
		}
	}
}

int
xsetcolorname(int index, const char *name)
{
	if (!BETWEEN(index, 0, (int)paletteCount - 1) || !name)
		return 1;
	MacColor invalid = {-1, -1, -1, -1};
	MacColor parsed = macos_parse_color(name, invalid);
	if (parsed.r < 0)
		return 1;
	palette[index] = parsed;
	tfulldirt();
	macos_request_redraw();
	return 0;
}

int
xgetcolor(int index, unsigned char *red, unsigned char *green,
		unsigned char *blue)
{
	if (!BETWEEN(index, 0, (int)paletteCount - 1))
		return 1;
	*red = (unsigned char)lrintf(palette[index].r * 255);
	*green = (unsigned char)lrintf(palette[index].g * 255);
	*blue = (unsigned char)lrintf(palette[index].b * 255);
	return 0;
}

static double
cellWidth(void)
{
	return MAX(1, win.cw);
}

static double
cellHeight(void)
{
	return MAX(1, win.ch);
}

static void
updateCellMetrics(void)
{
	win.cw = MAX(1, (int)ceil(mac_renderer_cell_width() * cwscale *
	    fontWidthSpacing));
	win.ch = MAX(1, (int)ceil(mac_renderer_cell_height() * chscale));
	if (nativeWindow)
		nativeWindow.contentResizeIncrements = NSMakeSize(win.cw, win.ch);
}

static double cellX(int x) { return borderpx + floor(x * cellWidth() + 0.5); }
static double cellY(int y) { return borderpx + floor(y * cellHeight() + 0.5); }
static double cellRight(int x, int wide) { return cellX(x + (wide ? 2 : 1)); }
static double rowBottom(int y) { return cellY(y + 1); }

void
xgetdimensions(int *width, int *height, int *cellwidth, int *cellheight)
{
	double scale = mac_renderer_scale();
	if (width) *width = (int)lrint(win.tw * scale);
	if (height) *height = (int)lrint(win.th * scale);
	if (cellwidth) *cellwidth = (int)lrint(win.cw * scale);
	if (cellheight) *cellheight = (int)lrint(win.ch * scale);
}

int xgraphicsavailable(void) { return 1; }

static double
rowBaseline(int y)
{
	double top = cellY(y), bottom = rowBottom(y);
	double lineHeight = mac_renderer_ascent() + mac_renderer_descent();
	double extra = MAX(0.0, bottom - top - lineHeight);
	return top + ceil(extra / 2.0) + mac_renderer_ascent();
}

int xdrawrowtop(int row) { return (int)cellY(MAX(0, row)); }
int xdrawrowbottom(int row) { return (int)rowBottom(MAX(0, row)); }

static void
resolveGlyph(Glyph glyph, int x, int y, MacColor *foreground,
		MacColor *background)
{
	uint32_t fg = glyph.fg, bg = glyph.bg;
	MacColor resolvedFg, resolvedBg, temporary;
	(void)x;
	if ((glyph.mode & ATTR_BOLD_FAINT) == ATTR_BOLD && BETWEEN(fg, 0, 7))
		fg += 8;
	resolvedFg = indexedColor(fg);
	resolvedBg = indexedColor(bg);
	if (IS_SET(MODE_REVERSE)) {
		if (fg == defaultfg)
			resolvedFg = indexedColor(defaultbg);
		else {
			resolvedFg.r = 1.0f - resolvedFg.r;
			resolvedFg.g = 1.0f - resolvedFg.g;
			resolvedFg.b = 1.0f - resolvedFg.b;
		}
		if (bg == defaultbg)
			resolvedBg = indexedColor(defaultfg);
		else {
			resolvedBg.r = 1.0f - resolvedBg.r;
			resolvedBg.g = 1.0f - resolvedBg.g;
			resolvedBg.b = 1.0f - resolvedBg.b;
		}
	}
	if (y == vimnav_curline_y() && glyph.bg == defaultbg)
		resolvedBg = indexedColor(vimnav_curline_bg);
	if (debug_mode && glyph.bg == defaultbg) {
		int start, end;
		vimnav_prompt_line_range(&start, &end);
		if (start >= 0 && BETWEEN(y, start, end))
			resolvedBg = indexedColor(debug_prompt_bg);
	}
	if (glyph.mode & ATTR_SELECTED)
		resolvedBg = indexedColor(selectionbg);
	if (glyph.mode & ATTR_MATCH) {
		resolvedFg = resolvedBg;
		resolvedBg = indexedColor(search_match_bg);
	}
	if ((glyph.mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
		resolvedFg.r *= 0.5f;
		resolvedFg.g *= 0.5f;
		resolvedFg.b *= 0.5f;
	}
	if (glyph.mode & ATTR_REVERSE) {
		temporary = resolvedFg;
		resolvedFg = resolvedBg;
		resolvedBg = temporary;
	}
	if ((glyph.mode & ATTR_BLINK) && IS_SET(MODE_BLINK))
		resolvedFg = resolvedBg;
	if (glyph.mode & ATTR_INVISIBLE)
		resolvedFg = resolvedBg;
	*foreground = resolvedFg;
	*background = resolvedBg;
}

static void
drawCell(Glyph glyph, int x, int y, int overlay, int applyHighlights)
{
	MacColor fg, bg;
	if (glyph.mode == ATTR_WDUMMY)
		return;
	if (applyHighlights && selection_active() && selected(x, y))
		glyph.mode |= ATTR_SELECTED;
	if (applyHighlights && search_active() && search_matched(x, y))
		glyph.mode |= ATTR_MATCH;
	resolveGlyph(glyph, x, y, &fg, &bg);
	double left = cellX(x), top = cellY(y);
	double width = cellRight(x, glyph.mode & ATTR_WIDE) - left;
	double height = rowBottom(y) - top;
	enum MacRenderLayer bgLayer = overlay ? MAC_LAYER_OVERLAY_BACKGROUND :
	    MAC_LAYER_BACKGROUND;
	enum MacRenderLayer textLayer = overlay ? MAC_LAYER_OVERLAY_TEXT :
	    MAC_LAYER_TEXT;
	enum MacRenderLayer decoLayer = overlay ? MAC_LAYER_OVERLAY_DECORATION :
	    MAC_LAYER_DECORATION;
	MacColor clear = indexedColor(IS_SET(MODE_REVERSE) ? defaultfg : defaultbg);
	if (bg.r != clear.r || bg.g != clear.g || bg.b != clear.b || bg.a != clear.a)
		mac_renderer_rect(bgLayer, left, top, width, height, bg);
	if (glyph.u != ' ' && glyph.u != 0) {
		unsigned int style = 0;
		if (glyph.mode & ATTR_BOLD) style |= MAC_FONT_BOLD;
		if (glyph.mode & ATTR_ITALIC) style |= MAC_FONT_ITALIC;
		if (glyph.mode & ATTR_EMOJI) style |= MAC_FONT_EMOJI;
		mac_renderer_rune(textLayer, glyph.u, style, left, top,
		    rowBaseline(y), width, height, fg);
	}
	if (glyph.mode & ATTR_UNDERLINE)
		mac_renderer_rect(decoLayer, left, rowBaseline(y) + 1,
		    cellRight(x, 0) - left, 1, fg);
	if (glyph.mode & ATTR_STRUCK)
		mac_renderer_rect(decoLayer, left, top + height / 2,
		    cellRight(x, 0) - left, 1, fg);
}

int
xstartdraw(void)
{
	/* Keep the last completed drawable visible until DEC mode 2026 ends. */
	if (IS_SET(MODE_SYNC))
		return 0;
	if (!drawingFrame) {
		macos_request_redraw();
		return 0;
	}
	MacColor clear = indexedColor(IS_SET(MODE_REVERSE) ? defaultfg : defaultbg);
	if (!mac_renderer_begin(clear))
		return 0;
	/* CAMetalLayer rotates drawables; rebuild a complete correct frame. */
	tfulldirt();
	return 1;
}

void
xdrawline(Line line, int x1, int y, int x2)
{
	for (int x = x1; x < x2; x++)
		drawCell(line[x], x, y, 0, 1);

	if (debug_mode) {
		int start, end;
		vimnav_prompt_line_range(&start, &end);
		if (start >= 0 && BETWEEN(y, start, end)) {
			const char *label = "     prompt line";
			int length = (int)strlen(label);
			double x = cellX(MAX(0, tlinelen(y)));
			double width = mac_renderer_text_width(label, length, 1.0);
			if (x + width <= win.w - borderpx) {
				mac_renderer_rect(MAC_LAYER_DECORATION, x, cellY(y),
				    width, rowBottom(y) - cellY(y),
				    indexedColor(debug_prompt_bg));
				mac_renderer_text(MAC_LAYER_DECORATION, label, length, x,
				    rowBaseline(y), 1.0, indexedColor(debug_prompt_fg));
			}
		}
	}
}

void
xdrawcursor(int cx, int cy, Glyph glyph, int ox, int oy, Glyph oldGlyph)
{
	(void)ox; (void)oy; (void)oldGlyph;
	int terminalOwned = vimnav_terminal_owned();
	if ((IS_SET(MODE_HIDE) && !terminalOwned) || cmdline_active())
		return;
	double left = cellX(cx), top = cellY(cy);
	double width = cellRight(cx, 0) - left;
	double height = rowBottom(cy) - top;
	MacColor color;
	glyph.mode &= ATTR_BOLD | ATTR_ITALIC | ATTR_UNDERLINE |
	    ATTR_STRUCK | ATTR_WIDE | ATTR_EMOJI;
	if (terminalOwned) {
		glyph.fg = defaultbg;
		glyph.bg = TRUECOLOR(0xff, 0x6b, 0x6b);
		color = (MacColor){1.0f, 0.42f, 0.42f, 1};
	} else if (IS_SET(MODE_REVERSE)) {
		glyph.mode |= ATTR_REVERSE;
		glyph.bg = defaultfg;
		if (selected(cx, cy)) {
			glyph.fg = defaultrcs;
			color = indexedColor(defaultcs);
		} else {
			glyph.fg = defaultcs;
			color = indexedColor(defaultrcs);
		}
	} else {
		glyph.fg = defaultbg;
		glyph.bg = defaultcs;
		color = indexedColor(defaultcs);
	}

	if (IS_SET(MODE_FOCUSED)) {
		switch (win.cursor) {
		case 7: glyph.u = 0x2603; /* fallthrough */
		case 0: case 1: case 2:
			drawCell(glyph, cx, cy, 1, 0);
			break;
		case 3: case 4:
			mac_renderer_rect(MAC_LAYER_OVERLAY_DECORATION, left,
			    top + height - cursorthickness, width,
			    cursorthickness, color);
			break;
		case 5: case 6:
			mac_renderer_rect(MAC_LAYER_OVERLAY_DECORATION, left, top,
			    cursorthickness, height, color);
			break;
		}
	} else {
		mac_renderer_rect(MAC_LAYER_OVERLAY_DECORATION, left, top, width, 1, color);
		mac_renderer_rect(MAC_LAYER_OVERLAY_DECORATION, left, top, 1, height, color);
		mac_renderer_rect(MAC_LAYER_OVERLAY_DECORATION, left + width - 1,
		    top, 1, height, color);
		mac_renderer_rect(MAC_LAYER_OVERLAY_DECORATION, left,
		    top + height - 1, width, 1, color);
	}
}

static void drawMarkedText(void);

static void
drawGraphicsPlacement(const GraphicsPlacementView *placement, void *context)
{
	int stage = (int)(intptr_t)context;
	double scale = mac_renderer_scale();
	double x = cellX(placement->column) + placement->pixel_x / scale;
	double y = cellY(placement->row) + placement->pixel_y / scale;
	double width = placement->natural_size ? placement->source_width / scale :
	    cellX(placement->column + placement->columns) -
	    cellX(placement->column);
	double height = placement->natural_size ? placement->source_height / scale :
	    cellY(placement->row + placement->rows) - cellY(placement->row);

	if (x >= borderpx + win.tw || x + width <= borderpx ||
	    y >= borderpx + win.th || y + height <= borderpx)
		return;
	mac_renderer_image(stage, placement->serial, placement->rgba,
	    placement->image_width, placement->image_height,
	    placement->source_x, placement->source_y,
	    placement->source_width, placement->source_height,
	    x, y, width, height);
	if (placement->selected || (selection_active() &&
	    selectedregion(placement->column, placement->row,
	    placement->columns, placement->rows))) {
		MacColor color = indexedColor(selectionbg);
		color.a = 0.45f;
		mac_renderer_rect(MAC_LAYER_OVERLAY_DECORATION, x, y, width,
		    height, color);
	}
}

static void
freeGraphicsImage(uint64_t serial, void *context)
{
	(void)context;
	mac_renderer_remove_image(serial);
}

void
xfinishdraw(void)
{
	if (graphics_placement_count()) {
		tlineviewprepare();
		mac_renderer_set_image_clip(borderpx, borderpx, win.tw, win.th);
		for (int stage = 0; stage < 3; stage++)
			graphics_draw(tisaltscreen(), stage, win.cw, win.ch, trow(),
			    tlineviewrow, drawGraphicsPlacement,
			    (void *)(intptr_t)stage);
	}
	sshind_draw();
	notif_draw();
	cmdline_draw();
	drawMarkedText();
	mac_renderer_end();
}

int xgpuactive(void) { return 1; }
int xgpuenabled(void) { return 1; }
void xsetgraphicsmode(int set) { (void)set; }

int
xgetcursor(void)
{
	return win.cursor;
}

int
xsetcursor(int cursor)
{
	if (!BETWEEN(cursor, 0, 7))
		return 1;
	win.cursor = cursor;
	return 0;
}

void
xsetmode(int set, unsigned int flags)
{
	int old = win.mode;
	MODBIT(win.mode, set, flags);
	if (!set && (flags & MODE_PASTEEVENT))
		[pasteboard5522 invalidate];
	if ((old ^ win.mode) & (MODE_REVERSE | MODE_MOUSE | MODE_HIDE)) {
		tfulldirt();
		macos_request_redraw();
	}
	if ((old ^ win.mode) & MODE_MOUSE)
		xsetmousecursor(IS_SET(MODE_MOUSE) ? 1 : 0);
	if ((old ^ win.mode) & MODE_SYNC) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		syncupdate_set(&syncUpdate, set, &now);
		if (set) {
			unsigned long generation = syncUpdate.generation;
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
			    (int64_t)synctimeout * NSEC_PER_MSEC),
			    dispatch_get_main_queue(), ^{
				if (syncUpdate.active &&
				    syncUpdate.generation == generation)
					xsetmode(0, MODE_SYNC);
			});
		} else {
			/* Present all mutations accumulated since BSU as one frame. */
			tfulldirt();
			macos_request_redraw();
		}
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

void xsetpointermotion(int set) { (void)set; }

void
xsetmousecursor(int shape)
{
	cursorKind = shape;
	[nativeWindow invalidateCursorRectsForView:nativeView];
}

void
xbell(void)
{
	if (!IS_SET(MODE_FOCUSED))
		[NSApp requestUserAttention:NSInformationalRequest];
	if (bellvolume)
		NSBeep();
}

void
xsettitle(char *title)
{
	DEFAULT(title, opt_title);
	if (title && *title)
		nativeWindow.title = [NSString stringWithUTF8String:title];
}

void
xcleanup(void)
{
	macos_pty_stop();
	stopControlSocket();
}

void
xseticontitle(char *title)
{
	DEFAULT(title, opt_title);
	if (title && *title)
		nativeWindow.miniwindowTitle = [NSString stringWithUTF8String:title];
}

void
xsetcwd(char *cwd)
{
	if (!cwd || !*cwd)
		return;
	NSString *path = [NSString stringWithUTF8String:cwd];
	nativeWindow.representedURL = [NSURL fileURLWithPath:path isDirectory:YES];
}

void xsetdwmsaveargv(const char *value) { (void)value; }

void
xximspot(int x, int y)
{
	imeCol = x;
	imeRow = y;
	[nativeView.inputContext invalidateCharacterCoordinates];
}

void
xsetsel(char *selection)
{
	if (!selection)
		return;
	free(primarySelection);
	primarySelection = selection;
	primaryImage = nil;
}

void
xsetimage(unsigned char *png, size_t length)
{
	primaryImage = nil;
	if (!png || !length) {
		free(png);
		return;
	}
	primaryImage = [NSData dataWithBytesNoCopy:png length:length
	    freeWhenDone:YES];
}

static void
writePaste(const char *text)
{
	if (!text)
		return;
	size_t length = strlen(text);
	char *copy = xmalloc(length + 1);
	memcpy(copy, text, length + 1);
	for (size_t i = 0; i < length; i++)
		if (copy[i] == '\n')
			copy[i] = '\r';
	if (tisvimnav_paste())
		while (length && copy[length - 1] == '\r')
			length--;
	if (IS_SET(MODE_BRCKTPASTE)) ttywrite("\033[200~", 6, 0);
	ttywrite(copy, length, 1);
	if (IS_SET(MODE_BRCKTPASTE)) ttywrite("\033[201~", 6, 0);
	free(copy);
	vimnav_paste_done();
}

static void
pasteboard5522Write(const char *data, size_t length, void *context)
{
	(void)context;
	ttywrite(data, length, 0);
}

static STPasteboard5522 *
nativePasteboard5522(void)
{
	if (!pasteboard5522)
		pasteboard5522 = [[STPasteboard5522 alloc]
		    initWithPasteboard:NSPasteboard.generalPasteboard
		    writer:pasteboard5522Write context:NULL];
	return pasteboard5522;
}

void
xclip5522read(const Clip5522Request *request)
{
	[nativePasteboard5522() readRequest:request];
}

static void
x5522paste(void)
{
	[nativePasteboard5522() beginPasteEvent];
}

static void
clipcopy(const Arg *unused)
{
	(void)unused;
	if (!primarySelection && !primaryImage)
		return;
	NSPasteboard *pasteboard = NSPasteboard.generalPasteboard;
	NSMutableArray<NSPasteboardType> *types = [NSMutableArray array];
	if (primarySelection)
		[types addObject:NSPasteboardTypeString];
	if (primaryImage)
		[types addObject:NSPasteboardTypePNG];
	[pasteboard declareTypes:types owner:nil];
	if (primarySelection)
		[pasteboard setString:[NSString stringWithUTF8String:primarySelection]
		    forType:NSPasteboardTypeString];
	if (primaryImage)
		[pasteboard setData:primaryImage forType:NSPasteboardTypePNG];
}

void xclipcopy(void) { clipcopy(NULL); }

void
clippaste(const Arg *unused)
{
	(void)unused;
	if (IS_SET(MODE_PASTEEVENT)) {
		x5522paste();
		return;
	}
	NSString *value = [NSPasteboard.generalPasteboard
	    stringForType:NSPasteboardTypeString];
	writePaste(value.UTF8String);
}

static void
selpaste(const Arg *unused)
{
	(void)unused;
	writePaste(primarySelection);
}

static void numlock(const Arg *unused) { (void)unused; win.mode ^= MODE_NUMLOCK; }
static void ttysend(const Arg *arg) { ttywrite(arg->s, strlen(arg->s), 1); }

static void
zoomabs(const Arg *arg)
{
	usedfontsize = MAX(1.0, arg->f);
	mac_renderer_set_font(usedfont, usedfontsize);
	updateCellMetrics();
	nativeResize(nativeView.bounds.size.width, nativeView.bounds.size.height);
	tfulldirt();
	macos_request_redraw();
}

static void zoom(const Arg *arg) { zoomabs(&(Arg){.f = usedfontsize + arg->f}); }
static void zoomreset(const Arg *unused) { (void)unused; zoomabs(&(Arg){.f = defaultFontSize}); }

static int
match(uint mask, uint state)
{
	return mask == XK_ANY_MOD || mask == (state & ~ignoremod);
}

static char *
kmap(KeySym symbol, uint state)
{
	int i;
	for (i = 0; i < (int)LEN(mappedkeys); i++)
		if (mappedkeys[i] == symbol)
			break;
	if (i == (int)LEN(mappedkeys) && (symbol & 0xffff) < 0xfd00)
		return NULL;
	for (Key *entry = key; entry < key + LEN(key); entry++) {
		if (entry->k != symbol || !match(entry->mask, state))
			continue;
		if (IS_SET(MODE_APPKEYPAD) ? entry->appkey < 0 : entry->appkey > 0)
			continue;
		if (IS_SET(MODE_NUMLOCK) && entry->appkey == 2)
			continue;
		if (IS_SET(MODE_APPCURSOR) ? entry->appcursor < 0 : entry->appcursor > 0)
			continue;
		return entry->s;
	}
	return NULL;
}

static int
kittyModifier(uint state)
{
	int modifier = 1;
	if (state & ShiftMask) modifier += 1;
	if (state & Mod1Mask) modifier += 2;
	if (state & ControlMask) modifier += 4;
	if (state & Mod4Mask) modifier += 8;
	return modifier;
}

static long
decodeUTF8(const char *buffer, int length)
{
	const unsigned char *s = (const unsigned char *)buffer;
	if (length <= 0) return 0;
	if (s[0] < 0x80) return s[0];
	if (length >= 2 && (s[0] & 0xe0) == 0xc0)
		return ((s[0] & 0x1f) << 6) | (s[1] & 0x3f);
	if (length >= 3 && (s[0] & 0xf0) == 0xe0)
		return ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) |
		    (s[2] & 0x3f);
	if (length >= 4 && (s[0] & 0xf8) == 0xf0)
		return ((s[0] & 7) << 18) | ((s[1] & 0x3f) << 12) |
		    ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
	return 0;
}

static long
kittyKeycode(KeySym symbol, uint state, const char *buffer, int length)
{
	if ((state & ControlMask) && BETWEEN(symbol, 'A', 'Z'))
		return 'a' + symbol - 'A';
	if ((state & ControlMask) && BETWEEN(symbol, 'a', 'z'))
		return symbol;
	long decoded = decodeUTF8(buffer, length);
	if (decoded) return decoded;
	switch (symbol) {
	case XK_Return: return 13;
	case XK_Tab: return 9;
	case XK_BackSpace: return 127;
	case XK_Escape: return 27;
	case XK_space: return 32;
	default:
		if (BETWEEN(symbol, 0x20, 0xff)) return symbol;
		return 0;
	}
}

static int
kittyWrite(KeySym symbol, uint state, const char *buffer, int length,
		int event)
{
	long code = kittyKeycode(symbol, state, buffer, length);
	char sequence[128];
	if (!code) return 0;
	int size = snprintf(sequence, sizeof(sequence), "\033[%ld;%d:%du",
	    code, kittyModifier(state), event);
	if (size <= 0 || size >= (int)sizeof(sequence)) return 0;
	ttywrite(sequence, size, 1);
	return 1;
}

static uint
nativeModifiers(NSEvent *event)
{
	NSEventModifierFlags flags = event.modifierFlags &
	    NSEventModifierFlagDeviceIndependentFlagsMask;
	uint state = 0;
	if (flags & NSEventModifierFlagShift) state |= ShiftMask;
	if (flags & NSEventModifierFlagControl) state |= ControlMask;
	if (flags & NSEventModifierFlagOption) state |= Mod1Mask;
	if (flags & NSEventModifierFlagCommand) state |= Mod4Mask;
	if (flags & NSEventModifierFlagCapsLock) state |= LockMask;
	if (flags & NSEventModifierFlagNumericPad) state |= Mod2Mask;
	return state;
}

static KeySym
nativeKeysym(NSEvent *event)
{
	switch (event.keyCode) {
	case 36: return XK_Return;
	case 48: return (event.modifierFlags & NSEventModifierFlagShift) ?
	    XK_ISO_Left_Tab : XK_Tab;
	case 51: return XK_BackSpace;
	case 53: return XK_Escape;
	case 114: return XK_Insert;
	case 115: return XK_Home;
	case 116: return XK_Prior;
	case 117: return XK_Delete;
	case 119: return XK_End;
	case 121: return XK_Next;
	case 123: return XK_Left;
	case 124: return XK_Right;
	case 125: return XK_Down;
	case 126: return XK_Up;
	case 122: return XK_F1; case 120: return XK_F2;
	case 99: return XK_F3; case 118: return XK_F4;
	case 96: return XK_F5; case 97: return XK_F6;
	case 98: return XK_F7; case 100: return XK_F8;
	case 101: return XK_F9; case 109: return XK_F10;
	case 103: return XK_F11; case 111: return XK_F12;
	case 105: return XK_F13; case 107: return XK_F14;
	case 113: return XK_F15; case 106: return XK_F16;
	case 64: return XK_F17; case 79: return XK_F18;
	case 80: return XK_F19; case 90: return XK_F20;
	case 76: return XK_KP_Enter;
	case 82: return XK_KP_0; case 83: return XK_KP_1;
	case 84: return XK_KP_2; case 85: return XK_KP_3;
	case 86: return XK_KP_4; case 87: return XK_KP_5;
	case 88: return XK_KP_6; case 89: return XK_KP_7;
	case 91: return XK_KP_8; case 92: return XK_KP_9;
	case 65: return XK_KP_Decimal; case 67: return XK_KP_Multiply;
	case 69: return XK_KP_Add; case 75: return XK_KP_Divide;
	case 78: return XK_KP_Subtract;
	default: break;
	}
	NSString *characters = event.charactersIgnoringModifiers;
	if (!characters.length)
		return NoSymbol;
	unichar character = [characters characterAtIndex:0];
	if (character >= 0xf700)
		return NoSymbol;
	if ((event.modifierFlags & NSEventModifierFlagShift) &&
	    BETWEEN(character, 'a', 'z'))
		character -= 'a' - 'A';
	return character;
}

static int
handleKeyEvent(KeySym symbol, uint state, const char *buffer, int length,
		int eventType, int repeat)
{
	if (IS_SET(MODE_KBDLOCK)) return 1;
	if (eventType == 3)
		return IS_SET(MODE_KITTYKBD) ?
		    kittyWrite(symbol, state, buffer, length, 3) : 1;
	if (cmdline_active()) {
		cmdline_handle_key(symbol, state, buffer, length);
		return 1;
	}
	if (symbol == XK_Shift_R && rightshiftseq && *rightshiftseq &&
	    !(state & ~(ShiftMask | LockMask | Mod2Mask))) {
		ttywrite(rightshiftseq, strlen(rightshiftseq), 1);
		return 1;
	}
	if (symbol == XK_Escape && (state & ShiftMask)) {
		if (vimnav.forced) vimnav_exit();
		else if (!tisvimnav()) vimnav_force_enter();
		else { vimnav.forced = 1; tfulldirt(); }
		macos_request_redraw();
		return 1;
	}
	if (tisvimnav() && vimnav_handle_key(symbol, state)) {
		macos_request_redraw();
		return 1;
	}
	if (!tisaltscreen() && (state & ControlMask)) {
		const char *insert = NULL;
		switch (symbol) {
		case XK_1: insert = "\xe2\x86\x90"; break;
		case XK_2: insert = "\xe2\x80\xa2"; break;
		case XK_3: insert = "\xe2\x86\x92"; break;
		case XK_4: insert = "<F17>"; break;
		case XK_5: insert = "<F18>"; break;
		case XK_6: insert = "<F19>"; break;
		case XK_7: insert = "<F20>"; break;
		case XK_8: insert = "<F21>"; break;
		case XK_9: insert = "\xe2\x80\xa6"; break;
		case XK_0: insert = "\xe2\x80\x93"; break;
		case XK_minus: insert = "\xe2\x80\x94"; break;
		}
		if (insert) { ttywrite(insert, strlen(insert), 1); return 1; }
	}
	/* A TUI that enabled DEC 5522 owns bare Ctrl+V as a rich paste event. */
	if (IS_SET(MODE_PASTEEVENT) && (symbol == 'v' || symbol == 'V') &&
	    (state & (ControlMask | ShiftMask)) == ControlMask &&
	    !(state & ~(ControlMask | LockMask | Mod2Mask))) {
		x5522paste();
		return 1;
	}
	if ((state & Mod4Mask) && (symbol == 'c' || symbol == 'C')) {
		clipcopy(NULL); return 1;
	}
	if ((state & Mod4Mask) && (symbol == 'v' || symbol == 'V')) {
		clippaste(NULL); return 1;
	}
	for (Shortcut *shortcut = shortcuts;
	    shortcut < shortcuts + LEN(shortcuts); shortcut++) {
		if (symbol == shortcut->keysym && match(shortcut->mod, state)) {
			shortcut->func(&shortcut->arg);
			return 1;
		}
	}
	char *mapped = kmap(symbol, state);
	if (mapped) {
		ttywrite(mapped, strlen(mapped), 1);
		return 1;
	}
	if (IS_SET(MODE_KITTYKBD) && kittyWrite(symbol, state, buffer,
	    length, repeat ? 2 : 1))
		return 1;
	/* AppKit routes Escape and Tab through doCommandBySelector: instead of
	 * insertText:. Emit the bytes XLookupString supplies on the X11 backend,
	 * while leaving ordinary and composed text on the NSTextInput path. */
	if (symbol == XK_Escape) {
		ttywrite("\033", 1, 1);
		return 1;
	}
	if (symbol == XK_Tab) {
		ttywrite("\t", 1, 1);
		return 1;
	}
	if (!tisaltscreen() && (state & ControlMask)) {
		if (symbol == 'e' || symbol == 'E') { kscrolldown(&(Arg){.i=1}); return 1; }
		if (symbol == 'y' || symbol == 'Y') { kscrollup(&(Arg){.i=1}); return 1; }
		if (symbol == 'u' || symbol == 'U') { kscrollup(&(Arg){.i=26}); return 1; }
		if (symbol == 'd' || symbol == 'D') { kscrolldown(&(Arg){.i=26}); return 1; }
		if (symbol == 'b' || symbol == 'B') { kscrollup(&(Arg){.i=52}); return 1; }
		if (symbol == 'f' || symbol == 'F') { kscrolldown(&(Arg){.i=52}); return 1; }
	}
	if (!length)
		return 0;
	if (state & Mod4Mask)
		return 0;
	if (state & (ControlMask | Mod1Mask)) {
		if ((state & Mod1Mask) && !IS_SET(MODE_8BIT))
			ttywrite("\033", 1, 1);
		ttywrite(buffer, length, 1);
		return 1;
	}
	return 0;
}

static int
eventColumn(NSEvent *event)
{
	NSPoint point = [nativeView convertPoint:event.locationInWindow fromView:nil];
	int column = (int)((point.x - borderpx) / cellWidth());
	int columns = win.cw ? MAX(1, win.tw / win.cw) : 1;
	LIMIT(column, 0, columns - 1);
	return column;
}

static int
eventRow(NSEvent *event)
{
	NSPoint point = [nativeView convertPoint:event.locationInWindow fromView:nil];
	int row = (int)((point.y - borderpx) / cellHeight());
	LIMIT(row, 0, MAX(0, trow() - 1));
	return row;
}

static uint
buttonMask(uint button)
{
	return button == Button1 ? Button1Mask :
	    button == Button2 ? Button2Mask :
	    button == Button3 ? Button3Mask :
	    button == Button4 ? Button4Mask :
	    button == Button5 ? Button5Mask : 0;
}

static int
mouseAction(uint button, uint state, int release)
{
	state &= ~buttonMask(button);
	for (MouseShortcut *shortcut = mshortcuts;
	    shortcut < mshortcuts + LEN(mshortcuts); shortcut++) {
		if (shortcut->release == (uint)release && shortcut->button == button &&
		    (match(shortcut->mod, state) ||
		    match(shortcut->mod, state & ~forcemousemod))) {
			shortcut->func(&shortcut->arg);
			return 1;
		}
	}
	return 0;
}

static int
selectionType(uint state)
{
	state &= ~(Button1Mask | forcemousemod);
	for (int type = 1; type < (int)LEN(selmasks); type++)
		if (match(selmasks[type], state))
			return type;
	return SEL_REGULAR;
}

static void
mouseReport(int type, uint button, uint state, int x, int y)
{
	static int oldX, oldY;
	int code = 0, selectedButton = button;
	char buffer[40];
	if (type == 2) {
		if (x == oldX && y == oldY) return;
		if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY)) return;
		if (IS_SET(MODE_MOUSEMOTION) && pressedButtons == 0) return;
		for (selectedButton = 1; selectedButton <= 11 &&
		    !(pressedButtons & (1U << (selectedButton - 1))); selectedButton++);
		code = 32;
	} else {
		if (button < 1 || button > 11) return;
		if (type == 1 && IS_SET(MODE_MOUSEX10)) return;
		if (type == 1 && (button == 4 || button == 5)) return;
	}
	oldX = x; oldY = y;
	if ((!IS_SET(MODE_MOUSESGR) && type == 1) || selectedButton == 12)
		code += 3;
	else if (selectedButton >= 8) code += 128 + selectedButton - 8;
	else if (selectedButton >= 4) code += 64 + selectedButton - 4;
	else code += selectedButton - 1;
	if (!IS_SET(MODE_MOUSEX10))
		code += (state & ShiftMask ? 4 : 0) +
		    (state & Mod1Mask ? 8 : 0) +
		    (state & ControlMask ? 16 : 0);
	int length;
	if (IS_SET(MODE_MOUSESGR))
		length = snprintf(buffer, sizeof(buffer), "\033[<%d;%d;%d%c",
		    code, x + 1, y + 1, type == 1 ? 'm' : 'M');
	else if (x < 223 && y < 223)
		length = snprintf(buffer, sizeof(buffer), "\033[M%c%c%c",
		    32 + code, 32 + x + 1, 32 + y + 1);
	else return;
	ttywrite(buffer, length, 0);
}

@implementation STMetalView

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)becomeFirstResponder { return YES; }

- (instancetype)initWithFrame:(NSRect)frame device:(id<MTLDevice>)device
{
	self = [super initWithFrame:frame device:device];
	if (self) {
		self.delegate = self;
		self.paused = YES;
		self.enableSetNeedsDisplay = YES;
		self.autoResizeDrawable = YES;
		self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
		self.framebufferOnly = YES;
		self.clearColor = MTLClearColorMake(0, 0, 0, 1);
		self.marked = [NSMutableAttributedString new];
		NSTrackingArea *tracking = [[NSTrackingArea alloc]
		    initWithRect:NSZeroRect
		    options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
		        NSTrackingInVisibleRect | NSTrackingCursorUpdate
		    owner:self userInfo:nil];
		[self addTrackingArea:tracking];
	}
	return self;
}

- (void)drawInMTKView:(MTKView *)view
{
	(void)view;
	drawingFrame = 1;
	draw();
	drawingFrame = 0;
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size
{
	(void)size;
	if (ttyfd >= 0)
		nativeResize(view.bounds.size.width, view.bounds.size.height);
}

- (void)keyDown:(NSEvent *)event { nativeHandleKey(event, 0); }
- (void)keyUp:(NSEvent *)event { nativeHandleKey(event, 1); }

- (void)flagsChanged:(NSEvent *)event
{
	static BOOL rightShiftDown;
	if (event.keyCode == 60) {
		BOOL down = (event.modifierFlags & NSEventModifierFlagShift) != 0;
		if (down && !rightShiftDown)
			handleKeyEvent(XK_Shift_R, nativeModifiers(event), "", 0, 1, 0);
		rightShiftDown = down;
	}
}

- (void)insertText:(id)value replacementRange:(NSRange)range
{
	(void)range;
	NSString *string = [value isKindOfClass:NSAttributedString.class] ?
	    [value string] : value;
	NSData *data = [string dataUsingEncoding:NSUTF8StringEncoding];
	if (data.length)
		ttywrite(data.bytes, data.length, 1);
	[self unmarkText];
}

- (void)doCommandBySelector:(SEL)selector { (void)selector; }
- (BOOL)hasMarkedText { return self.marked.length > 0; }
- (NSRange)markedRange { return self.hasMarkedText ? NSMakeRange(0, self.marked.length) : NSMakeRange(NSNotFound, 0); }
- (NSRange)selectedRange { return self.markedSelection; }
- (void)setMarkedText:(id)value selectedRange:(NSRange)selected replacementRange:(NSRange)replacement
{
	(void)replacement;
	NSAttributedString *text = [value isKindOfClass:NSAttributedString.class] ?
	    value : [[NSAttributedString alloc] initWithString:value];
	[self.marked setAttributedString:text];
	self.markedSelection = selected;
	macos_request_redraw();
}
- (void)unmarkText { [self.marked.mutableString setString:@""]; macos_request_redraw(); }
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText { return @[]; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual
{
	if (actual) *actual = NSMakeRange(NSNotFound, 0);
	(void)range; return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point { (void)point; return NSNotFound; }
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual
{
	(void)range;
	if (actual) *actual = NSMakeRange(0, 0);
	NSRect rect = NSMakeRect(cellX(imeCol), cellY(imeRow),
	    cellWidth(), cellHeight());
	rect = [self convertRect:rect toView:nil];
	return [self.window convertRectToScreen:rect];
}

- (void)copy:(id)sender { (void)sender; clipcopy(NULL); }
- (void)paste:(id)sender { (void)sender; clippaste(NULL); }
- (void)selectAll:(id)sender
{
	(void)sender;
	selstart(0, 0, SNAP_LINE);
	selextend(MAX(0, win.tw / MAX(1, win.cw) - 1), MAX(0, trow() - 1),
	    SEL_REGULAR, 1);
	size_t pngLength = 0;
	unsigned char *png = getselimage(&pngLength);
	xsetsel(getsel());
	xsetimage(png, pngLength);
	clipcopy(NULL);
	macos_request_redraw();
}

- (void)mouseDown:(NSEvent *)event
{
	uint state = nativeModifiers(event), button = Button1;
	pressedButtons |= 1U << (button - 1);
	int x = eventColumn(event), y = eventRow(event);
	if (IS_SET(MODE_MOUSE) && !(state & forcemousemod)) {
		mouseReport(0, button, state, x, y); return;
	}
	if (mouseAction(button, state, 0)) return;
	int snap = event.clickCount >= 3 ? SNAP_LINE :
	    event.clickCount == 2 ? SNAP_WORD : 0;
	selstart(x, y, snap);
	macos_request_redraw();
}

- (void)mouseDragged:(NSEvent *)event
{
	uint state = nativeModifiers(event);
	int x = eventColumn(event), y = eventRow(event);
	if (IS_SET(MODE_MOUSE) && !(state & forcemousemod))
		mouseReport(2, Button1, state, x, y);
	else {
		selextend(x, y, selectionType(state), 0);
		macos_request_redraw();
	}
}

- (void)mouseUp:(NSEvent *)event
{
	uint state = nativeModifiers(event), button = Button1;
	pressedButtons &= ~(1U << (button - 1));
	int x = eventColumn(event), y = eventRow(event);
	if (IS_SET(MODE_MOUSE) && !(state & forcemousemod)) {
		mouseReport(1, button, state, x, y); return;
	}
	if (mouseAction(button, state, 1)) return;
	selextend(x, y, selectionType(state), 1);
	size_t pngLength = 0;
	unsigned char *png = getselimage(&pngLength);
	xsetsel(getsel());
	xsetimage(png, pngLength);
	macos_request_redraw();
}

- (void)rightMouseDown:(NSEvent *)event
{
	uint button = Button3, state = nativeModifiers(event);
	pressedButtons |= 1U << (button - 1);
	if (IS_SET(MODE_MOUSE) && !(state & forcemousemod))
		mouseReport(0, button, state, eventColumn(event), eventRow(event));
	else mouseAction(button, state, 0);
}

- (void)rightMouseDragged:(NSEvent *)event
{
	uint state = nativeModifiers(event);
	if (IS_SET(MODE_MOUSE) && !(state & forcemousemod))
		mouseReport(2, Button3, state, eventColumn(event), eventRow(event));
}

- (void)rightMouseUp:(NSEvent *)event
{
	uint button = Button3, state = nativeModifiers(event);
	pressedButtons &= ~(1U << (button - 1));
	if (IS_SET(MODE_MOUSE) && !(state & forcemousemod))
		mouseReport(1, button, state, eventColumn(event), eventRow(event));
	else mouseAction(button, state, 1);
}

- (void)otherMouseDown:(NSEvent *)event
{
	uint button = event.buttonNumber == 2 ? Button2 : Button3;
	uint state = nativeModifiers(event);
	pressedButtons |= 1U << (button - 1);
	if (IS_SET(MODE_MOUSE) && !(state & forcemousemod))
		mouseReport(0, button, state, eventColumn(event), eventRow(event));
	else mouseAction(button, state, 0);
}

- (void)otherMouseUp:(NSEvent *)event
{
	uint button = event.buttonNumber == 2 ? Button2 : Button3;
	uint state = nativeModifiers(event);
	pressedButtons &= ~(1U << (button - 1));
	if (IS_SET(MODE_MOUSE) && !(state & forcemousemod))
		mouseReport(1, button, state, eventColumn(event), eventRow(event));
	else mouseAction(button, state, 1);
}

- (void)otherMouseDragged:(NSEvent *)event
{
	uint button = event.buttonNumber == 2 ? Button2 : Button3;
	uint state = nativeModifiers(event);
	if (IS_SET(MODE_MOUSE) && !(state & forcemousemod))
		mouseReport(2, button, state, eventColumn(event), eventRow(event));
}

- (void)mouseMoved:(NSEvent *)event
{
	if (IS_SET(MODE_MOUSEMOTION) || IS_SET(MODE_MOUSEMANY))
		mouseReport(2, 12, nativeModifiers(event), eventColumn(event), eventRow(event));
}

- (void)scrollWheel:(NSEvent *)event
{
	double delta = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : event.deltaY;
	if (fabs(delta) < 0.01) return;
	uint button = delta > 0 ? Button4 : Button5;
	uint state = nativeModifiers(event);
	int steps = MAX(1, (int)ceil(fabs(delta) / (event.hasPreciseScrollingDeltas ? 8.0 : 1.0)));
	steps = MIN(steps, 8);
	for (int i = 0; i < steps; i++) {
		if (IS_SET(MODE_MOUSE) && !(state & forcemousemod))
			mouseReport(0, button, state, eventColumn(event), eventRow(event));
		else if (!mouseAction(button, state, 0))
			(button == Button4 ? kscrollup : kscrolldown)(&(Arg){.i=1});
	}
	macos_request_redraw();
}

- (void)resetCursorRects
{
	NSCursor *cursor = cursorKind == 1 ? NSCursor.arrowCursor :
	    cursorKind == 2 ? NSCursor.pointingHandCursor : NSCursor.IBeamCursor;
	[self addCursorRect:self.bounds cursor:cursor];
}

@end

static void
nativeHandleKey(NSEvent *event, int release)
{
	KeySym symbol = nativeKeysym(event);
	uint state = nativeModifiers(event);
	NSString *characters = (state & Mod1Mask) ?
	    (event.charactersIgnoringModifiers ?: @"") : (event.characters ?: @"");
	NSData *data = [characters dataUsingEncoding:NSUTF8StringEncoding];
	const char *bytes = data.length ? data.bytes : "";
	int consumed = handleKeyEvent(symbol, state, bytes, (int)data.length,
	    release ? 3 : 1, event.isARepeat);
	if (!release && !consumed)
		[nativeView interpretKeyEvents:@[event]];
	macos_request_redraw();
}

static void
drawMarkedText(void)
{
	if (!nativeView.marked.length || cmdline_active()) return;
	NSString *text = nativeView.marked.string;
	NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
	double x = cellX(imeCol), y = cellY(imeRow);
	double width = mac_renderer_text_width(data.bytes, data.length, 1.0);
	mac_renderer_rect(MAC_LAYER_OVERLAY_BACKGROUND, x, y, width,
	    cellHeight(), indexedColor(defaultbg));
	mac_renderer_text(MAC_LAYER_OVERLAY_TEXT, data.bytes, data.length, x,
	    rowBaseline(imeRow), 1.0, indexedColor(defaultfg));
	mac_renderer_rect(MAC_LAYER_OVERLAY_DECORATION, x, rowBottom(imeRow) - 1,
	    width, 1, indexedColor(defaultcs));
}

static void
scheduleWindowReveal(void)
{
	if (!nativeWindow || windowRevealed)
		return;
	NSUInteger generation = ++windowRevealGeneration;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC),
	    dispatch_get_main_queue(), ^{
		if (generation != windowRevealGeneration || shuttingDown)
			return;
		nativeWindow.alphaValue = 1.0;
		windowRevealed = 1;
	});
}

static void
updateWindowReveal(void)
{
	if (!managedWindowReveal) {
		scheduleWindowReveal();
		return;
	}
	if (!nativeWindow || windowRevealed ||
	    !macos_managed_reveal_ready(managedWindowRevealRequested,
	        nativeWindow.isKeyWindow))
		return;
	nativeWindow.alphaValue = 1.0;
	windowRevealed = 1;
}

@interface STAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation STAppDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{ (void)sender; return YES; }
- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app
{ (void)app; return YES; }
- (void)applicationWillTerminate:(NSNotification *)note
{ (void)note; cleanupNative(); }
- (void)windowWillClose:(NSNotification *)note
{ (void)note; if (!shuttingDown) [NSApp terminate:nil]; }
- (void)windowDidBecomeKey:(NSNotification *)note
{ (void)note; nativeFocus(1); updateWindowReveal(); }
- (void)windowDidResignKey:(NSNotification *)note
{ (void)note; nativeFocus(0); }
- (void)windowDidMove:(NSNotification *)note
{ (void)note; updateWindowReveal(); }
- (void)windowDidResize:(NSNotification *)note
{ (void)note; updateWindowReveal(); }
- (void)windowDidChangeBackingProperties:(NSNotification *)note
{
	(void)note;
	mac_renderer_set_scale(nativeWindow.backingScaleFactor);
	updateCellMetrics();
	nativeResize(nativeView.bounds.size.width, nativeView.bounds.size.height);
	updateWindowReveal();
}
@end

static void
nativeFocus(int focused)
{
	MODBIT(win.mode, focused, MODE_FOCUSED);
	if (IS_SET(MODE_FOCUS))
		ttywrite(focused ? "\033[I" : "\033[O", 3, 0);
	tfulldirt();
	macos_request_redraw();
}

void
macos_request_redraw(void)
{
	if (!nativeView || redrawPending || IS_SET(MODE_SYNC)) return;
	redrawPending = 1;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
	    (int64_t)(MAX(0.0, minlatency) * NSEC_PER_MSEC)),
	    dispatch_get_main_queue(), ^{
		redrawPending = 0;
		[nativeView setNeedsDisplay:YES];
	});
}

void
macos_draw_overlay_rect(double x, double y, double width, double height,
		MacColor color)
{
	mac_renderer_rect(MAC_LAYER_OVERLAY_BACKGROUND, x, y, width, height, color);
}

void
macos_draw_overlay_text(const char *text, size_t len, double x,
		double baseline, double fontScale, MacColor color)
{
	mac_renderer_text(MAC_LAYER_OVERLAY_TEXT, text, len, x, baseline,
	    fontScale, color);
}

double
macos_measure_text(const char *text, size_t len, double fontScale)
{
	return mac_renderer_text_width(text, len, fontScale);
}

void
macos_draw_cmdline_snapshot(const MacCmdlineSnapshot *snapshot)
{
	MacColor bg = macos_parse_color(cmdline_bg_color, indexedColor(defaultbg));
	MacColor fg = macos_parse_color(cmdline_fg_color, indexedColor(defaultfg));
	MacColor error = macos_parse_color(cmdline_err_color, fg);
	MacColor cursor = macos_parse_color(cmdline_cursor_color, indexedColor(defaultcs));
	MacColor border = macos_parse_color(cmdline_border_color, fg);
	MacColor selectedColor = macos_parse_color(cmdline_sel_color, bg);
	mac_renderer_rect(MAC_LAYER_OVERLAY_BACKGROUND, snapshot->x,
	    snapshot->y, snapshot->width, snapshot->height, bg);
	mac_renderer_rect(MAC_LAYER_OVERLAY_DECORATION, snapshot->x,
	    snapshot->y, snapshot->width, cmdline_border_top, border);
	double x = win.cw / 2.0;
	double baseline = snapshot->y + snapshot->baseline;
	if (snapshot->state == 1) {
		mac_renderer_text(MAC_LAYER_OVERLAY_TEXT, &snapshot->prefix, 1,
		    x, baseline, 1.0, fg);
		x += win.cw;
		if (snapshot->mode == 2 && snapshot->input_len > 0) {
			int start = MIN(snapshot->anchor, snapshot->cursor);
			int end = MAX(snapshot->anchor, snapshot->cursor) + 1;
			while (end < snapshot->input_len &&
			    ((unsigned char)snapshot->input[end] & 0xc0) == 0x80) end++;
			double sx = x + mac_renderer_text_width(snapshot->input, start, 1.0);
			double ex = x + mac_renderer_text_width(snapshot->input, end, 1.0);
			mac_renderer_rect(MAC_LAYER_OVERLAY_BACKGROUND, sx,
			    snapshot->y + snapshot->content_y, ex - sx,
			    snapshot->content_height, selectedColor);
		}
		if (snapshot->input_len)
			mac_renderer_text(MAC_LAYER_OVERLAY_TEXT, snapshot->input,
			    snapshot->input_len, x, baseline, 1.0, fg);
		double cursorX = x + mac_renderer_text_width(snapshot->input,
		    MAX(0, snapshot->cursor), 1.0);
		if (snapshot->mode == 0) {
			mac_renderer_rect(MAC_LAYER_OVERLAY_DECORATION, cursorX,
			    snapshot->y + snapshot->content_y, 2,
			    snapshot->content_height, cursor);
		} else {
			mac_renderer_rect(MAC_LAYER_OVERLAY_BACKGROUND, cursorX,
			    snapshot->y + snapshot->content_y, win.cw,
			    snapshot->content_height, cursor);
			if (snapshot->cursor < snapshot->input_len) {
				int length = 1;
				while (snapshot->cursor + length < snapshot->input_len &&
				    ((unsigned char)snapshot->input[snapshot->cursor + length] & 0xc0) == 0x80)
					length++;
				mac_renderer_text(MAC_LAYER_OVERLAY_TEXT,
				    snapshot->input + snapshot->cursor, length,
				    cursorX, baseline, 1.0, bg);
			}
		}
	} else if (snapshot->state == 2) {
		mac_renderer_text(MAC_LAYER_OVERLAY_TEXT, snapshot->error,
		    strlen(snapshot->error), x, baseline, 1.0, error);
	}
}

int sshind_active(void) { return sshIndicator.active; }
int sshind_height(void) { return sshIndicator.active ? sshIndicator.height + 2 * sshind_border_width : 0; }

void
sshind_show(const char *host)
{
	if (!host) return;
	strlcpy(sshIndicator.host, host, sizeof(sshIndicator.host));
	double scale = sshind_font_scale;
	sshIndicator.width = (int)ceil(mac_renderer_text_width(host, strlen(host), scale)) +
	    2 * sshind_padding + 2 * sshind_border_width;
	sshIndicator.height = (int)ceil((mac_renderer_ascent() + mac_renderer_descent()) * scale) +
	    2 * sshind_padding + 2 * sshind_border_width;
	sshIndicator.active = 1;
	macos_request_redraw();
}

void sshind_hide(void) { memset(&sshIndicator, 0, sizeof(sshIndicator)); macos_request_redraw(); }
void sshind_resize(void) { macos_request_redraw(); }

void
sshind_draw(void)
{
	if (!sshIndicator.active) return;
	double x = win.w - sshIndicator.width - sshind_margin;
	double y = sshind_margin;
	MacColor border = macos_parse_color(sshind_border_color, indexedColor(defaultcs));
	MacColor bg = macos_parse_color(sshind_bg_color, indexedColor(defaultbg));
	MacColor fg = macos_parse_color(sshind_fg_color, indexedColor(defaultfg));
	mac_renderer_rect(MAC_LAYER_OVERLAY_BACKGROUND, x, y,
	    sshIndicator.width, sshIndicator.height, border);
	mac_renderer_rect(MAC_LAYER_OVERLAY_BACKGROUND,
	    x + sshind_border_width, y + sshind_border_width,
	    sshIndicator.width - 2 * sshind_border_width,
	    sshIndicator.height - 2 * sshind_border_width, bg);
	double baseline = y + sshind_border_width + sshind_padding +
	    mac_renderer_ascent() * sshind_font_scale;
	mac_renderer_text(MAC_LAYER_OVERLAY_TEXT, sshIndicator.host,
	    strlen(sshIndicator.host), x + sshind_border_width + sshind_padding,
	    baseline, sshind_font_scale, fg);
}

static void
parseToastLines(NativeToast *toast)
{
	toast->lineCount = 0;
	int length = (int)strlen(toast->msg), offset = 0;
	while (offset <= length && toast->lineCount < NOTIF_MAX_LINES) {
		char *newline = strchr(toast->msg + offset, '\n');
		toast->lineOffset[toast->lineCount] = offset;
		toast->lineLength[toast->lineCount] = newline ?
		    (int)(newline - (toast->msg + offset)) : length - offset;
		toast->lineCount++;
		if (!newline) break;
		offset = (int)(newline - toast->msg) + 1;
	}
}

static void
parseToastOptions(NativeToast *toast, const char *raw, const char **body)
{
	toast->timeoutMs = notif_display_ms;
	toast->fontScale = notif_font_scale;
	toast->fg = macos_parse_color(notif_fg_color, indexedColor(defaultfg));
	toast->bg = macos_parse_color(notif_bg_color, indexedColor(defaultbg));
	toast->border = macos_parse_color(notif_border_color, indexedColor(defaultcs));
	const char *separator = strchr(raw, NOTIF_META_SEP);
	if (!separator) { *body = raw; return; }
	*body = separator + 1;
	const char *cursor = raw;
	while (cursor < separator) {
		const char *end = memchr(cursor, NOTIF_META_DELIM, separator - cursor);
		if (!end) end = separator;
		const char *equal = memchr(cursor, '=', end - cursor);
		if (equal) {
			char key[16] = {0}, value[64] = {0};
			size_t keyLength = MIN((size_t)(equal - cursor), sizeof(key) - 1);
			size_t valueLength = MIN((size_t)(end - equal - 1), sizeof(value) - 1);
			memcpy(key, cursor, keyLength);
			memcpy(value, equal + 1, valueLength);
			if (!strcmp(key, "t")) toast->timeoutMs = MAX(1, atoi(value));
			else if (!strcmp(key, "fg")) toast->fg = macos_parse_color(value, toast->fg);
			else if (!strcmp(key, "bg")) toast->bg = macos_parse_color(value, toast->bg);
			else if (!strcmp(key, "b")) toast->border = macos_parse_color(value, toast->border);
			else if (!strcmp(key, "ts")) toast->fontScale = MAX(0.25, atof(value) / usedfontsize);
		}
		cursor = end + 1;
	}
}

void
notif_show(const char *raw)
{
	if (!raw) return;
	if (notifications.count == NOTIF_MAX_TOASTS)
		notifications.count--;
	memmove(&notifications.toasts[1], &notifications.toasts[0],
	    notifications.count * sizeof(NativeToast));
	notifications.count++;
	NativeToast *toast = &notifications.toasts[0];
	memset(toast, 0, sizeof(*toast));
	const char *body;
	parseToastOptions(toast, raw, &body);
	strlcpy(toast->msg, body, sizeof(toast->msg));
	parseToastLines(toast);
	double maxWidth = 0;
	for (int i = 0; i < toast->lineCount; i++)
		maxWidth = MAX(maxWidth, mac_renderer_text_width(
		    toast->msg + toast->lineOffset[i], toast->lineLength[i],
		    toast->fontScale));
	toast->width = (int)ceil(maxWidth) + 2 * notif_padding + 2 * notif_border_width;
	toast->height = (int)ceil(toast->lineCount *
	    (mac_renderer_ascent() + mac_renderer_descent()) * toast->fontScale) +
	    2 * notif_padding + 2 * notif_border_width;
	clock_gettime(CLOCK_MONOTONIC, &toast->shown);
	toast->active = 1;
	macos_request_redraw();
}

void notif_hide(void) { memset(&notifications, 0, sizeof(notifications)); macos_request_redraw(); }
int notif_active(void) { return notifications.count > 0; }

int
notif_check_timeout(struct timespec *now)
{
	int minimum = -1;
	for (int i = notifications.count - 1; i >= 0; i--) {
		NativeToast *toast = &notifications.toasts[i];
		int remaining = toast->timeoutMs - (int)TIMEDIFF((*now), toast->shown);
		if (remaining <= 0) {
			memmove(toast, toast + 1,
			    (notifications.count - i - 1) * sizeof(*toast));
			notifications.count--;
		} else if (minimum < 0 || remaining < minimum) minimum = remaining;
	}
	return minimum;
}

void notif_resize(void) { macos_request_redraw(); }

void
notif_draw(void)
{
	double y = notif_margin + (sshind_active() ? sshind_height() + notif_margin : 0);
	for (int i = 0; i < notifications.count; i++) {
		NativeToast *toast = &notifications.toasts[i];
		double x = win.w - toast->width - notif_margin;
		mac_renderer_rect(MAC_LAYER_OVERLAY_BACKGROUND, x, y,
		    toast->width, toast->height, toast->border);
		mac_renderer_rect(MAC_LAYER_OVERLAY_BACKGROUND,
		    x + notif_border_width, y + notif_border_width,
		    toast->width - 2 * notif_border_width,
		    toast->height - 2 * notif_border_width, toast->bg);
		double baseline = y + notif_border_width + notif_padding +
		    mac_renderer_ascent() * toast->fontScale;
		for (int line = 0; line < toast->lineCount; line++) {
			mac_renderer_text(MAC_LAYER_OVERLAY_TEXT,
			    toast->msg + toast->lineOffset[line],
			    toast->lineLength[line],
			    x + notif_border_width + notif_padding, baseline,
			    toast->fontScale, toast->fg);
			baseline += (mac_renderer_ascent() + mac_renderer_descent()) *
			    toast->fontScale;
		}
		y += toast->height + notif_toast_gap;
	}
}

static void
nativeResize(double width, double height)
{
	win.w = MAX(1, (int)floor(width));
	win.h = MAX(1, (int)floor(height));
	int columns = MAX(1, (win.w - 2 * borderpx) / MAX(1, win.cw));
	int rows = MAX(1, (win.h - 2 * borderpx) / MAX(1, win.ch));
	win.tw = columns * win.cw;
	win.th = rows * win.ch;
	tresize(columns, rows);
	if (ttyfd >= 0) {
		double scale = mac_renderer_scale();
		ttyresize((int)lrint(win.tw * scale), (int)lrint(win.th * scale));
	}
	sshind_resize(); notif_resize(); cmdline_resize();
	tfulldirt();
	macos_request_redraw();
}

static void
buildMenu(void)
{
	NSMenu *menubar = [NSMenu new];
	NSMenuItem *appRoot = [NSMenuItem new];
	[menubar addItem:appRoot];
	NSMenu *appMenu = [NSMenu new];
	NSString *name = NSProcessInfo.processInfo.processName;
	[appMenu addItemWithTitle:[@"About " stringByAppendingString:name]
	    action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
	[appMenu addItem:NSMenuItem.separatorItem];
	[appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name]
	    action:@selector(terminate:) keyEquivalent:@"q"];
	appRoot.submenu = appMenu;

	NSMenuItem *editRoot = [[NSMenuItem alloc] initWithTitle:@"Edit"
	    action:nil keyEquivalent:@""];
	[menubar addItem:editRoot];
	NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
	[edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
	[edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
	[edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
	editRoot.submenu = edit;

	NSMenuItem *windowRoot = [[NSMenuItem alloc] initWithTitle:@"Window"
	    action:nil keyEquivalent:@""];
	[menubar addItem:windowRoot];
	NSMenu *windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
	[windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:)
	    keyEquivalent:@"m"];
	[windowMenu addItemWithTitle:@"Toggle Full Screen"
	    action:@selector(toggleFullScreen:) keyEquivalent:@"f"]
	    .keyEquivalentModifierMask = NSEventModifierFlagControl |
	    NSEventModifierFlagCommand;
	windowRoot.submenu = windowMenu;
	NSApp.windowsMenu = windowMenu;
	NSApp.mainMenu = menubar;
}

static void
initializeNativeWindow(int columns, int rows)
{
	usedfont = opt_font ?: (char *)macosDefaultFont;
	fontWidthSpacing = opt_font ? 1.0 : macosDefaultFontWidthSpacing;
	usedfontsize = parseFontSize(usedfont);
	defaultFontSize = usedfontsize;
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (!device) die("st: Metal is not available\n");
	nativeView = [[STMetalView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)
	    device:device];
	if (!mac_renderer_init((__bridge void *)nativeView, usedfont, usedfontsize))
		die("st: could not initialize Metal renderer\n");
	graphics_set_image_free_callback(freeGraphicsImage, NULL);
	updateCellMetrics();
	NSSize content = NSMakeSize(2 * borderpx + columns * win.cw,
	    2 * borderpx + rows * win.ch);
	NSRect rect = NSMakeRect(0, 0, content.width, content.height);
	NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
	nativeWindow = [[NSWindow alloc] initWithContentRect:rect styleMask:style
	    backing:NSBackingStoreBuffered defer:NO];
	nativeWindow.releasedWhenClosed = NO;
	nativeWindow.animationBehavior = NSWindowAnimationBehaviorNone;
	nativeWindow.alphaValue = 0.0;
	nativeWindow.contentView = nativeView;
	nativeWindow.contentResizeIncrements = NSMakeSize(win.cw, win.ch);
	nativeWindow.title = [NSString stringWithUTF8String:opt_title ?: "st"];
	nativeWindow.tabbingMode = NSWindowTabbingModeDisallowed;
	nativeWindow.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
	nativeWindow.delegate = appDelegate;
	[nativeWindow center];
	if (opt_has_position) {
		NSScreen *screen = NSScreen.mainScreen;
		NSRect frame = screen.visibleFrame;
		[nativeWindow setFrameTopLeftPoint:NSMakePoint(frame.origin.x + opt_x,
		    NSMaxY(frame) - opt_y)];
	}
	mac_renderer_set_scale(nativeWindow.backingScaleFactor);
	updateCellMetrics();
	win.mode = MODE_VISIBLE;
	win.cursor = cursorshape;
	xloadcols();
	/* Merely publish the window here.  Launch Services or the window manager
	 * decides whether this launch should take focus; forcing activation races
	 * AeroSpace and can pull the user back to this workspace after they leave. */
	[nativeWindow orderFront:nil];
	if (NSApp.isActive)
		[nativeWindow makeKeyWindow];
	[nativeWindow makeFirstResponder:nativeView];
	updateWindowReveal();
	setenv("WINDOWID", [[NSString stringWithFormat:@"%ld",
	    nativeWindow.windowNumber] UTF8String], 1);
}

static int
parseGeometry(const char *value)
{
	unsigned int c = cols, rws = rows;
	int x = 0, y = 0;
	int count = sscanf(value, "%ux%u%d%d", &c, &rws, &x, &y);
	if (count < 2) return 0;
	cols = MAX(1, c); rows = MAX(1, rws);
	if (count >= 4) { opt_x = x; opt_y = y; opt_has_position = 1; }
	return 1;
}

static void
usage(void)
{
	die("usage: %s [-adiv] [-c class] [-f font] [-g geometry] "
	    "[-n name] [-o file] [-T title] [-t title] "
	    "[--from-save dir] [[-e] command [args ...]]\n", argv0);
}

static dispatch_source_t
terminationSource(int number)
{
	signal(number, SIG_IGN);
	dispatch_source_t source = dispatch_source_create(
	    DISPATCH_SOURCE_TYPE_SIGNAL, number, 0, dispatch_get_main_queue());
	dispatch_source_set_event_handler(source, ^{ [NSApp terminate:nil]; });
	dispatch_resume(source);
	return source;
}

static void
setupManagedWindowRevealSource(void)
{
	if (!managedWindowReveal)
		return;
	signal(SIGUSR1, SIG_IGN);
	revealSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGUSR1,
	    0, dispatch_get_main_queue());
	dispatch_source_set_event_handler(revealSource, ^{
		managedWindowRevealRequested = 1;
		updateWindowReveal();
	});
	dispatch_resume(revealSource);
}

static void
setupSources(void)
{
	ttySource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, ttyfd, 0,
	    dispatch_get_main_queue());
	dispatch_source_set_event_handler(ttySource, ^{
		ttyread();
		macos_request_redraw();
	});
	dispatch_resume(ttySource);

	if (blinktimeout) {
		blinkSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
		    dispatch_get_main_queue());
		dispatch_source_set_timer(blinkSource,
		    dispatch_time(DISPATCH_TIME_NOW, blinktimeout * NSEC_PER_MSEC),
		    blinktimeout * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
		dispatch_source_set_event_handler(blinkSource, ^{
			if (tattrset(ATTR_BLINK)) {
				win.mode ^= MODE_BLINK;
				tsetdirtattr(ATTR_BLINK);
				macos_request_redraw();
			}
			if (childready()) reapchild();
		});
		dispatch_resume(blinkSource);
	}

	persistSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
	    dispatch_get_main_queue());
	dispatch_source_set_timer(persistSource,
	    dispatch_time(DISPATCH_TIME_NOW, (uint64_t)persistinterval * NSEC_PER_MSEC),
	    (uint64_t)persistinterval * NSEC_PER_MSEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(persistSource, ^{ if (persist_active()) persist_save(); });
	dispatch_resume(persistSource);

	notificationSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
	    dispatch_get_main_queue());
	dispatch_source_set_timer(notificationSource,
	    dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
	    50 * NSEC_PER_MSEC, 5 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(notificationSource, ^{
		if (notif_active()) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			int before = notifications.count;
			notif_check_timeout(&now);
			if (before != notifications.count) macos_request_redraw();
		}
		if (childready()) reapchild();
	});
	dispatch_resume(notificationSource);

	termSource = terminationSource(SIGTERM);
	interruptSource = terminationSource(SIGINT);
	hangupSource = terminationSource(SIGHUP);
	quitSource = terminationSource(SIGQUIT);
}

static void
handleControlMessage(const char *message, size_t length)
{
	const char *newline = memchr(message, '\n', length);
	if (!newline) return;
	size_t commandLength = newline - message;
	const char *payload = newline + 1;
	size_t payloadLength = length - commandLength - 1;
	char *copy = xmalloc(payloadLength + 1);
	memcpy(copy, payload, payloadLength); copy[payloadLength] = '\0';
	if (commandLength == 6 && !memcmp(message, "notify", 6)) notif_show(copy);
	else if (commandLength == 7 && !memcmp(message, "savecmd", 7))
		persist_set_save_cmd(copy);
	free(copy);
}

static void
startControlSocket(void)
{
	const char *home = getenv("HOME") ?: "/tmp";
	char directory[PATH_MAX];
	snprintf(directory, sizeof(directory), "%s/.runtime/st/st-%d", home, getpid());
	mkdir(directory, 0700);
	snprintf(controlPath, sizeof(controlPath), "%s/control.sock", directory);
	unlink(controlPath);
	controlFD = socket(AF_UNIX, SOCK_STREAM, 0);
	if (controlFD < 0) return;
	fcntl(controlFD, F_SETFL, O_NONBLOCK);
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	strlcpy(address.sun_path, controlPath, sizeof(address.sun_path));
	if (bind(controlFD, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    listen(controlFD, 8) < 0) {
		close(controlFD); controlFD = -1; return;
	}
	chmod(controlPath, 0600);
	controlSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, controlFD,
	    0, dispatch_get_main_queue());
	dispatch_source_set_event_handler(controlSource, ^{
		for (;;) {
			int client = accept(controlFD, NULL, NULL);
			if (client < 0) break;
			struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
			setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
			dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
				char buffer[4096]; size_t total = 0; ssize_t got;
				while (total < sizeof(buffer) &&
				    (got = read(client, buffer + total,
				        sizeof(buffer) - total)) > 0)
					total += got;
				close(client);
				if (total) {
					NSData *message = [NSData dataWithBytes:buffer length:total];
					dispatch_async(dispatch_get_main_queue(), ^{
						handleControlMessage(message.bytes, message.length);
					});
				}
			});
		}
	});
	dispatch_resume(controlSource);
}

static void
stopControlSocket(void)
{
	if (controlSource) { dispatch_source_cancel(controlSource); controlSource = nil; }
	if (controlFD >= 0) { close(controlFD); controlFD = -1; }
	if (*controlPath) { unlink(controlPath); controlPath[0] = '\0'; }
}

static void
cleanupNative(void)
{
	if (shuttingDown) return;
	shuttingDown = 1;
	xcleanup();
	if (persist_active()) { persist_save(); persist_cleanup(); }
	mac_renderer_destroy();
}

int
main(int argc, char *argv[])
{
	/* Parse native long options before suckless arg.h. */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--from-save") && i + 1 < argc) {
			opt_fromsave = argv[i + 1];
			memmove(&argv[i], &argv[i + 2], (argc - i - 1) * sizeof(*argv));
			argc -= 2; break;
		}
		if (!strcmp(argv[i], "--from-orphan")) {
			opt_fromorphan = 1;
			memmove(&argv[i], &argv[i + 1], (argc - i) * sizeof(*argv));
			argc--; break;
		}
	}

	ARGBEGIN {
	case 'a': allowaltscreen = 0; break;
	case 'c': opt_class = EARGF(usage()); break;
	case 'e': if (argc > 0) --argc, ++argv; goto run;
	case 'f': opt_font = EARGF(usage()); break;
	case 'g': if (!parseGeometry(EARGF(usage()))) usage(); break;
	case 'i': opt_fixed = 1; break;
	case 'o': opt_io = EARGF(usage()); break;
	case 'l': opt_line = EARGF(usage()); break;
	case 'n': opt_name = EARGF(usage()); break;
	case 't': case 'T': opt_title = EARGF(usage()); break;
	case 'w': opt_embed = EARGF(usage()); break;
	case 'd': debug_mode = 1; break;
	case 'v': die("%s " VERSION "\n", argv0); break;
	default: usage();
	} ARGEND;

run:
	(void)opt_class; (void)opt_embed; (void)opt_name; (void)opt_fixed;
	if (argc > 0) {
		opt_cmd = argv;
		if (opt_fromsave) persist_set_ephemeral(1);
		if (argc >= 3 && argv[1][0] == '-' && strchr(argv[1], 'c'))
			persist_set_altcmd(argv[2]);
		else {
			char command[PATH_MAX] = {0}; size_t used = 0;
			for (int i = 0; i < argc && used < sizeof(command) - 1; i++) {
				if (i) command[used++] = ' ';
				size_t n = MIN(strlen(argv[i]), sizeof(command) - used - 1);
				memcpy(command + used, argv[i], n); used += n;
			}
			persist_set_altcmd(command);
		}
	}
	if (!opt_title) opt_title = (opt_line || !opt_cmd) ? "st" : opt_cmd[0];
	if (!macos_init_utf8_locale())
		die("st: could not initialize a UTF-8 locale\n");
	cols = MAX(cols, 1); rows = MAX(rows, 1);
	tnew(cols, rows);
	if (opt_fromorphan && !opt_fromsave) {
		const char *orphan = persist_find_orphan();
		if (orphan) opt_fromsave = (char *)orphan;
	}
	if (opt_fromsave) persist_restore(opt_fromsave, &cols, &rows);

	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	managedWindowReveal = getenv("ST_AEROSPACE_MANAGED") != NULL;
	setupManagedWindowRevealSource();
	appDelegate = [STAppDelegate new];
	NSApp.delegate = appDelegate;
	buildMenu();
	initializeNativeWindow(cols, rows);
	NSString *resourceBin = [NSBundle.mainBundle.resourcePath
	    stringByAppendingPathComponent:@"bin"];
	if ([[NSFileManager defaultManager] fileExistsAtPath:resourceBin]) {
		NSString *oldPath = [NSString stringWithUTF8String:getenv("PATH") ?: ""];
		NSString *newPath = [NSString stringWithFormat:@"%@:%@", resourceBin, oldPath];
		setenv("PATH", newPath.UTF8String, 1);
	}
	persist_init(getpid());
	persist_register();
	selinit();
	ttyfd = ttynew(opt_line, shell, opt_io, opt_cmd);
	if (macos_pty_start(ttyfd) < 0)
		die("couldn't configure nonblocking PTY output: %s\n",
		    strerror(errno));
	nativeResize(nativeView.bounds.size.width, nativeView.bounds.size.height);
	cmdline_init();
	if (opt_fromsave && persist_get_altcmd()[0] && !persist_is_ephemeral()) {
		ttywrite(persist_get_altcmd(), strlen(persist_get_altcmd()), 1);
		ttywrite("\n", 1, 1);
	}
	startControlSocket();
	setupSources();
	macos_request_redraw();
	[NSApp run];
	cleanupNative();
	return 0;
}
