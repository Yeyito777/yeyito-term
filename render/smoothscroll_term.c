/* See LICENSE for license details. */
/* Smooth-scroll terminal glue: drives the engine in smoothscroll.c for two
 * kinds of motion and tells renderers, per grid row, how far to shift it and
 * what to clip it to.  See reference/smooth-scroll.md.
 *
 * - Scrollback: follows term.scr; every row shifts, clipped to the grid.
 * - Regions: an application scrolled a DECSTBM region (SU/SD, IND/RI at a
 *   margin, IL/DL at the top margin); only rows of that band shift, clipped
 *   to it.  The lines that left the band are kept as copies and slide out.
 *   st has no left/right margins, so apps with side panels scroll the whole
 *   width and repaint the panels in place; comparing the band before and
 *   after each batch finds the columns that really moved (see classify()).
 *   Full-screen apps (alternate screen) glide this way only when they ask
 *   for it (DECSCLM) or with smoothscroll_regions = 2.
 * - App-driven (CSI ? 2032 h, docs/app-smooth-scroll.md): the application
 *   declares the moving rectangles and sets their sub-row offset each frame;
 *   st only draws them shifted and keeps the lines its scrolls pushed out. */

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <wchar.h>

#include "../st.h"
#include "../win.h"
#include "../graphics.h"
#include "smoothscroll.h"

/* Precise input snaps to a row this long after the last event once the
 * fingers are up, so momentum events that follow keep the gesture alive. */
#define SS_SETTLE_MS 60.0

static struct {
	int enabled, regions, decsclm;
	double duration;
	SmoothScroll ss;
	/* Logical view at the previous sync. */
	int valid, alt, rows, cols, scr;
	Line top;
	/* The previous frame was drawn with a nonzero offset. */
	int moved, full;
} sst;

/* One animating scroll region (band of rows top..bot). */
static struct {
	int active, top, bot, cols, alt;
	int fresh;	/* start the clock at the next drawn frame */
	SmoothScroll ss;
	/* Copies of the lines that left the band, nearest first: above[0] sits
	 * just above the band top, below[0] just below its bottom. */
	Glyph **above, **below;
	int nabove, nbelow, cap;
	/* The band before the current batch of scrolls, the batch's net
	 * distance, and the columns [left, right) that move. */
	Glyph *snap;
	int batchn, left, right;
	/* Per column: VOTED (some cell told stayed/moved apart), BLANK (blank
	 * on every band row before and after the batch). */
	unsigned char *colinfo;
	/* Per band row: nonzero when the row scrolled rather than being
	 * rewritten, so its cells may vote on which columns move. */
	unsigned char *rowvotes;
} reg;

/* A copied line being drawn outside the grid: its shift relative to the
 * band edge row it is drawn as, and no highlights (it is not on the grid). */
static struct {
	int on;
	double shift;
	/* App-driven copies clip to their rectangle [top, end) x [left, right). */
	int app, top, end, left, right;
} ghost;

/* App-driven smooth scrolling (CSI ? 2032 h). */
#define APP_MAXRECTS 8
#define APP_MAXTERMS 32
typedef struct {
	int top, bot, left, right;	/* rows top..bot, columns [left, right) */
} AppRect;
static struct {
	int on;
	int nrects;
	AppRect rect[APP_MAXRECTS];
	/* The offset in rows (positive: drawn lower) is base plus easing terms
	 * timed by st, so the glide runs at the display's refresh whatever the
	 * pty delivers; offset is its value this frame. */
	double base, offset;
	struct { double rows, start, ms; } term[APP_MAXTERMS];
	int nterms;
	/* Lines the last scrolls of region gtop..gbot pushed out, nearest
	 * first: above[0] left over the top edge, below[0] over the bottom. */
	int gtop, gbot, gcols, cap;
	Glyph **above, **below;
	int nabove, nbelow;
	/* Geometry the state belongs to: a resize or screen switch clears it. */
	int rows, cols, alt;
	/* Rows app-mode region scrolls moved content (up positive), for the
	 * frame log: offset - scrolled is where the content is drawn. */
	long scrolled;
} app;

static void appclear(void);

static double
monotonicms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1E3 + ts.tv_nsec / 1E6;
}

/* Replaced by the unit tests to step time deterministically. */
static double (*nowms)(void) = monotonicms;

/* A copied line goes, with any images that went with it. */
static void
freecopy(Glyph *copy)
{
	graphics_recycle_line((Line)copy);
	free(copy);
}

static void
ghostsfree(Glyph **list, int *n, int from)
{
	int i;

	for (i = from; i < *n; i++)
		freecopy(list[i]);
	if (*n > from)
		*n = from;
}

static void
regreset(void)
{
	ghostsfree(reg.above, &reg.nabove, 0);
	ghostsfree(reg.below, &reg.nbelow, 0);
	free(reg.above);
	free(reg.below);
	free(reg.snap);
	free(reg.colinfo);
	free(reg.rowvotes);
	memset(&reg, 0, sizeof(reg));
}

void
smoothscroll_configure(int enabled, double duration_ms, int regions)
{
	regreset();
	appclear();
	app.on = 0;
	sst.enabled = enabled;
	sst.regions = regions;
	sst.decsclm = 0;
	sst.duration = duration_ms > 0 ? duration_ms : 0;
	sst.valid = 0;
	ss_reset(&sst.ss);
}

int
smoothscroll_enabled(void)
{
	return sst.enabled;
}

/* DECSCLM (CSI ? 4 h): an application asks for smooth scrolling.  It turns
 * region animation on even with smoothscroll_regions = 0; resetting it only
 * drops that request (routine terminal resets send CSI ? 4 l), it never
 * disables the configured default. */
void
smoothscroll_decsclm(int set)
{
	sst.decsclm = set;
}

static void
remember(int alt, int rows, int cols, int scr)
{
	sst.valid = 1;
	sst.alt = alt;
	sst.rows = rows;
	sst.cols = cols;
	sst.scr = scr;
	sst.top = tviewline(0);
}

/* Where is the line that was on row 0 at the last sync? */
static int
findtop(int delta, int rows, int *row)
{
	int y;

	if (!sst.top)
		return 0;
	if (tviewline(delta) == sst.top) {
		*row = delta;
		return 1;
	}
	for (y = -rows; y <= rows; y++) {
		if (tviewline(y) == sst.top) {
			*row = y;
			return 1;
		}
	}
	return 0;
}

/* Follow logical scroll changes made by anyone (wheel, keys, vimnav, search,
 * snap-to-prompt).  Content that moved only because output arrived while the
 * view was scrolled keeps its row 0 line and is not animated. */
static void
follow(double now)
{
	int alt = tisaltscreen(), rows = trow(), cols = tcol();
	int scr = tscrolled(), row = 0;

	if (reg.active && (alt != reg.alt || cols != reg.cols ||
	    reg.bot >= rows || scr != 0 || (sst.valid && rows != sst.rows)))
		regreset();
	/* Full-screen applications scroll themselves: no scrollback offsets. */
	if (!sst.valid || alt || alt != sst.alt || rows != sst.rows ||
	    cols != sst.cols) {
		ss_reset(&sst.ss);
		remember(alt, rows, cols, scr);
		return;
	}
	if (scr != sst.scr) {
		/* Jumps further than a screen (gg, G, search) glide in from one
		 * screen away in the direction of travel. */
		if (!findtop(scr - sst.scr, rows, &row))
			row = scr > sst.scr ? rows : -rows;
		row = MAX(-rows, MIN(rows, row));
		if (row != 0)
			ss_shift(&sst.ss, -row, now, sst.duration);
	}
	ss_limit(&sst.ss, thistlen() - scr, scr);
	remember(alt, rows, cols, scr);
}

static Glyph *
copyline(int y, int cols)
{
	Line line = tviewline(y);
	Glyph *copy = malloc(cols * sizeof(Glyph));

	if (copy && line)
		memcpy(copy, line, cols * sizeof(Glyph));
	else if (copy)
		memset(copy, 0, cols * sizeof(Glyph));
	return copy;
}

/* Drop the first `drop` copies, then put copies of the `n` departing rows
 * (y0, y0 + step, ...) first, keeping at most a band height of copies. */
static void
ghostspushn(Glyph **list, int *count, int cap, int cols, int drop, int y0,
		int step, int n)
{
	int i, keep;

	drop = MIN(drop, *count);
	for (i = 0; i < drop; i++)
		freecopy(list[i]);
	memmove(list, list + drop, (*count - drop) * sizeof(*list));
	*count -= drop;
	keep = MIN(*count, cap - n);
	ghostsfree(list, count, MAX(0, keep));
	memmove(list + n, list, *count * sizeof(*list));
	for (i = 0; i < n; i++)
		list[i] = copyline(y0 + i * step, cols);
	*count += n;
}

static void
ghostspush(Glyph **list, int *count, int drop, int y0, int step, int n)
{
	ghostspushn(list, count, reg.cap, reg.cols, drop, y0, step, n);
}

/* ---- App-driven smooth scrolling ---- */

static void
appghostsfree(void)
{
	ghostsfree(app.above, &app.nabove, 0);
	ghostsfree(app.below, &app.nbelow, 0);
	free(app.above);
	free(app.below);
	app.above = app.below = NULL;
	app.gtop = app.gbot = -1;
	app.gcols = app.cap = 0;
}

/* Offset, rectangles and copies go; the mode itself stays. */
static void
appclear(void)
{
	appghostsfree();
	app.nrects = 0;
	app.base = app.offset = 0.0;
	app.nterms = 0;
	app.rows = trow();
	app.cols = tcol();
	app.alt = tisaltscreen();
}

/* A scroll of region top..bot by n while the app drives: keep the lines it
 * pushed out so a shifted rectangle can show them past its edge. */
static void
appcapture(int top, int bot, int orig, int n)
{
	int height = bot - top + 1, distance = n < 0 ? -n : n, i;

	if (orig != top)
		return;	/* IL/DL below the top margin: an edit, not a scroll */
	app.scrolled += n;
	if (app.gtop != top || app.gbot != bot || app.gcols != tcol()) {
		appghostsfree();
		app.above = calloc(height, sizeof(*app.above));
		app.below = calloc(height, sizeof(*app.below));
		if (!app.above || !app.below) {
			appghostsfree();
			return;
		}
		app.gtop = top;
		app.gbot = bot;
		app.gcols = tcol();
		app.cap = height;
	}
	if (distance >= height) {	/* nothing adjacent survives */
		ghostsfree(app.above, &app.nabove, 0);
		ghostsfree(app.below, &app.nbelow, 0);
		return;
	}
	if (n > 0) {
		ghostspushn(app.above, &app.nabove, app.cap, app.gcols, 0,
		    top + n - 1, -1, distance);
		ghostspushn(app.below, &app.nbelow, app.cap, app.gcols,
		    distance, 0, 0, 0);
	} else {
		ghostspushn(app.below, &app.nbelow, app.cap, app.gcols, 0,
		    bot - distance + 1, 1, distance);
		ghostspushn(app.above, &app.nabove, app.cap, app.gcols,
		    distance, 0, 0, 0);
	}
	/* Images on the departing lines leave with them (the scroll would
	 * drop them).  Over the top only the rows that left go along: the
	 * rest of an image stays on the grid, where the app places it. */
	for (i = 0; i < distance; i++) {
		if (n > 0)
			graphics_detach_line(tviewline(top + n - 1 - i),
			    (Line)app.above[i], i + 1);
		else
			graphics_detach_line(tviewline(bot - distance + 1 + i),
			    (Line)app.below[i], 0);
	}
}

/* Grid row a copied line is drawn as (its images too), or INT_MIN. */
int
smoothscroll_copy_row(const void *line)
{
	int k;

	if (!app.on || !line)
		return INT_MIN;
	for (k = 0; k < app.nabove; k++)
		if ((const void *)app.above[k] == line)
			return app.gtop - 1 - k;
	for (k = 0; k < app.nbelow; k++)
		if ((const void *)app.below[k] == line)
			return app.gbot + 1 + k;
	return INT_MIN;
}

void
smoothscroll_app_mode(int set)
{
	regreset();
	appclear();
	app.on = set && sst.enabled;
}

int
smoothscroll_app_state(void)
{
	if (!sst.enabled)
		return -1;
	return app.on;
}

static int
parseint(const char *s, int *out)
{
	char *end;
	long v = strtol(s, &end, 10);

	if (end == s || v < INT_MIN || v > INT_MAX)
		return 0;
	*out = (int)v;
	return *end == '\0' || *end == ',';
}

/* The offset at time `now`: base plus what the easing terms still have
 * left, never past the lines st kept for that side.  At rest the copies
 * (and their images) have served their glide. */
static void
appoffset(double now)
{
	double o = app.base;
	int i, j;

	for (i = j = 0; i < app.nterms; i++) {
		double left = ss_ease((now - app.term[i].start) / app.term[i].ms);
		if (left == 0.0)
			continue;
		o += app.term[i].rows * left;
		app.term[j++] = app.term[i];
	}
	app.nterms = j;
	o = MIN(o, (double)app.nabove);
	o = MAX(o, -(double)app.nbelow);
	app.offset = o;
	if (o == 0.0 && app.nterms == 0 && app.base == 0.0) {
		ghostsfree(app.above, &app.nabove, 0);
		ghostsfree(app.below, &app.nbelow, 0);
	}
}

/* OSC 2032 ; r ; T,L,B,R ; ...   rectangles (1-based, inclusive)
 * OSC 2032 ; o ; N               offset N thousandths of a row, no easing
 * OSC 2032 ; m ; dA ; B ; D      move: offset += dA, plus B easing to 0
 *                                over D ms from now (st times it)
 * OSC 2032 ; c                   clear offset, rectangles and copies */
void
smoothscroll_app_osc(int narg, char **args)
{
	int i, v;

	if (!sst.enabled || !app.on || narg < 2)
		return;
	if (app.rows != trow() || app.cols != tcol() ||
	    app.alt != tisaltscreen())
		appclear();
	switch (args[1][0]) {
	case 'r':
		app.nrects = 0;
		for (i = 2; i < narg && app.nrects < APP_MAXRECTS; i++) {
			int t, l, b, r;
			const char *p = args[i];

			if (!parseint(p, &t) || !(p = strchr(p, ',')) ||
			    !parseint(++p, &l) || !(p = strchr(p, ',')) ||
			    !parseint(++p, &b) || !(p = strchr(p, ',')) ||
			    !parseint(++p, &r))
				continue;
			t = MAX(1, t);
			l = MAX(1, l);
			b = MIN(trow(), b);
			r = MIN(tcol(), r);
			if (t > b || l > r)
				continue;
			app.rect[app.nrects++] = (AppRect){ t - 1, b - 1,
			    l - 1, r };
		}
		break;
	case 'o':
		if (narg < 3 || !parseint(args[2], &v))
			return;
		v = MAX(-1000 * trow(), MIN(1000 * trow(), v));
		app.base = v / 1000.0;
		app.nterms = 0;
		appoffset(nowms());
		break;
	case 'm': {
		int b = 0, d = 0;
		if (narg < 3 || !parseint(args[2], &v))
			return;
		if (narg >= 5 && (!parseint(args[3], &b) || !parseint(args[4], &d)))
			return;
		v = MAX(-1000 * trow(), MIN(1000 * trow(), v));
		b = MAX(-1000 * trow(), MIN(1000 * trow(), b));
		app.base += v / 1000.0;
		if (b != 0 && d > 0) {
			if (app.nterms == APP_MAXTERMS) {	/* oldest jumps */
				memmove(app.term, app.term + 1,
				    (APP_MAXTERMS - 1) * sizeof(app.term[0]));
				app.nterms--;
			}
			app.term[app.nterms].rows = b / 1000.0;
			app.term[app.nterms].start = nowms();
			app.term[app.nterms].ms = MIN(d, 10000);
			app.nterms++;
		}
		appoffset(nowms());
		break;
	}
	case 'c':
		appclear();
		break;
	}
}

/* The image being drawn is anchored on this line (smoothscroll_image()). */
static const void *imageline;

/* The rectangle cell (x, y) moves with.  Rectangles also clip at rest, so
 * an image the app lets run past one (to have it while gliding) never
 * draws outside it.  An image on a copied line belongs to the rectangle
 * whose edge its line left through. */
static const AppRect *
apprect(int x, int y)
{
	int i, row;

	if (!app.on)
		return NULL;
	if (imageline && (row = smoothscroll_copy_row(imageline)) != INT_MIN) {
		for (i = 0; i < app.nrects; i++) {
			const AppRect *r = &app.rect[i];
			if (x >= r->left && x < r->right &&
			    (row < app.gtop ? r->top == app.gtop :
			    r->bot == app.gbot))
				return r;
		}
		return NULL;
	}
	for (i = 0; i < app.nrects; i++) {
		const AppRect *r = &app.rect[i];
		if (y >= r->top && y <= r->bot && x >= r->left && x < r->right)
			return r;
	}
	return NULL;
}

void
smoothscroll_image(const void *anchor)
{
	imageline = anchor;
}

static void
drawappghost(Glyph *line, const AppRect *r, int asrow, double shift)
{
	int right = MIN(r->right, app.gcols);

	if (right <= r->left)
		return;
	ghost.on = 1;
	ghost.app = 1;
	ghost.shift = shift;
	ghost.top = r->top;
	ghost.end = r->bot + 1;
	ghost.left = r->left;
	ghost.right = r->right;
	xdrawline(line, r->left, asrow, right);
	ghost.on = ghost.app = 0;
}

/* Past a shifted rectangle's edge: the lines the scroll pushed out, when the
 * edge is the scrolled region's edge; otherwise background. */
static void
appdrawextra(void)
{
	double o = app.offset;
	int i, k;

	if (!app.on || o == 0.0)
		return;
	for (i = 0; i < app.nrects; i++) {
		const AppRect *r = &app.rect[i];
		if (o > 0 && r->top == app.gtop)
			for (k = 1; k <= app.nabove && k < o + 1; k++)
				drawappghost(app.above[k - 1], r, r->top, o - k);
		if (o < 0 && r->bot == app.gbot)
			for (k = 1; k <= app.nbelow && k + o < 1; k++)
				drawappghost(app.below[k - 1], r, r->bot, o + k);
	}
}

/* Called by tscrollup()/tscrolldown() before rows [orig, bot] of the region
 * [top, bot] move by n (positive: content moves up). */
void
smoothscroll_region(int top, int bot, int orig, int n)
{
	int alt, height, distance;
	double now;

	if (!sst.enabled || !n)
		return;
	if (app.on) {	/* the application animates; keep what leaves */
		appcapture(top, bot, orig, n);
		return;
	}
	alt = tisaltscreen();
	/* Full-screen applications glide automatically only when they ask
	 * (DECSCLM) or with smoothscroll_regions = 2: guessing which of their
	 * cells move tears side panels and re-wrapped text.  The app-driven
	 * protocol (CSI ? 2032 h) is the way for them to glide. */
	if (!sst.decsclm && sst.regions < (alt ? 2 : 1))
		return;
	if (!ss_region_animates(alt, trow(), top, bot, orig, n) ||
	    tscrolled() != 0 || sst.ss.offset != 0.0) {
		/* A redraw-sized scroll of the gliding band snaps it. */
		if (reg.active && orig <= reg.bot && bot >= reg.top &&
		    (orig == top || (n < 0 ? -n : n) > bot - orig))
			regreset();
		return;
	}
	if (reg.active && (reg.top != top || reg.bot != bot ||
	    reg.cols != tcol() || reg.alt != alt))
		regreset();
	height = bot - top + 1;
	distance = n < 0 ? -n : n;
	if (!reg.active) {
		reg.active = 1;
		reg.top = top;
		reg.bot = bot;
		reg.cols = tcol();
		reg.alt = alt;
		reg.cap = height;
		reg.left = 0;
		reg.right = reg.cols;
		reg.above = calloc(height, sizeof(*reg.above));
		reg.below = calloc(height, sizeof(*reg.below));
		reg.snap = malloc((size_t)height * reg.cols * sizeof(Glyph));
		reg.colinfo = malloc(reg.cols);
		reg.rowvotes = malloc(height);
		if (!reg.above || !reg.below || !reg.snap || !reg.colinfo ||
		    !reg.rowvotes) {
			regreset();
			return;
		}
	}
	/* A new batch: remember the band as it was before it. */
	if (!reg.fresh) {
		for (int y = 0; y < height; y++) {
			Line line = tviewline(top + y);
			Glyph *dst = reg.snap + (size_t)y * reg.cols;
			if (line)
				memcpy(dst, line, reg.cols * sizeof(Glyph));
			else
				memset(dst, 0, reg.cols * sizeof(Glyph));
		}
		reg.batchn = 0;
	}
	reg.batchn += n;
	/* Lines re-entering from the other side are redrawn by the app as
	 * grid rows, so their copies go; the departing rows are copied. */
	if (n > 0) {	/* rows top..top+n-1 leave over the top */
		ghostspush(reg.above, &reg.nabove, 0, top + n - 1, -1, distance);
		ghostspush(reg.below, &reg.nbelow, distance, 0, 0, 0);
	} else {	/* rows bot-n+1..bot leave over the bottom */
		ghostspush(reg.below, &reg.nbelow, 0, bot - distance + 1, 1,
		    distance);
		ghostspush(reg.above, &reg.nabove, distance, 0, 0, 0);
	}
	now = nowms();
	/* Until a frame presents the glide (one frame, or a whole synchronized
	 * update), scrolls add up without the clock running. */
	if (reg.fresh)
		reg.ss.start = now;
	ss_shift(&reg.ss, n, now, sst.duration);
	/* Accumulated scrolls past a whole region (fast output) just jump. */
	if (reg.ss.state != SS_ANIMATING ||
	    (reg.ss.from < 0 ? -reg.ss.from : reg.ss.from) > height) {
		regreset();
		return;
	}
	reg.fresh = 1;
}

enum { COL_VOTED = 1, COL_BLANK = 2 };

static int
glyphblank(const Glyph *g)
{
	return g->u == ' ' || g->u == 0;
}

/* The widest run of all-blank columns in [from, to); 0 if there is none. */
static int
widestgap(int from, int to, int *g0, int *g1)
{
	int x, start = -1, best = 0;

	for (x = from; x <= to; x++) {
		if (x < to && (reg.colinfo[x] & COL_BLANK)) {
			if (start < 0)
				start = x;
			continue;
		}
		if (start >= 0 && x - start > best) {
			best = x - start;
			*g0 = start;
			*g1 = x;
		}
		start = -1;
	}
	return best > 0;
}

static int
glypheq(const Glyph *a, const Glyph *b)
{
	return a->u == b->u && a->mode == b->mode && a->fg == b->fg &&
	    a->bg == b->bg;
}

/* Which columns of the band really moved in the last batch?  For each cell,
 * compare the current grid with the band before the batch at the same row
 * (it stayed: a side panel repainted in place) and at the row it came from
 * (it moved).  Cells where both predictions agree (blank areas, borders) say
 * nothing.  A column moves when "moved" wins its vote; the moving span runs
 * from the first to the last such column.  Neighbouring columns that told
 * nothing apart (text repeated on every row, padding) look the same either
 * way, but belong somewhere: a run of them up to the row edge moves; a run
 * between moving and static columns is split at its widest all-blank gap
 * (the padding between panes), so a pane's repeated prefix moves with it and
 * a panel's border and labels stay with the panel.  Everything else stays.
 * Columns repainted with new content (a panel that re-wraps) match neither
 * and stay put too.  Returns 0 when the band was mostly rewritten rather
 * than scrolled: then it should not animate at all. */
int
smoothscroll_classify(void)
{
	int height = reg.bot - reg.top + 1, cols = reg.cols, n = reg.batchn;
	int x, y, left = -1, right = -1;
	long moved = 0, other = 0, informative = 0, rewritten = 0;

	if (!reg.snap || n == 0)
		return 1;
	/* Rows the app rewrote instead of scrolling (a cursor-line highlight,
	 * text reflowed beside a floating panel) would vote "stayed" in every
	 * column where they happen to match their old contents, pinning those
	 * columns of every row in place.  Only rows that scrolled vote. */
	for (y = 0; y < height; y++) {
		int from = y + n;
		Line line = tviewline(reg.top + y);
		long m = 0, o = 0;

		reg.rowvotes[y] = 0;
		if (!line || from < 0 || from >= height)
			continue;
		for (x = 0; x < cols; x++) {
			const Glyph *same = reg.snap + (size_t)y * cols + x;
			const Glyph *src = reg.snap + (size_t)from * cols + x;

			if (glypheq(same, src) || glypheq(&line[x], same))
				continue;
			if (glypheq(&line[x], src))
				m++;
			else
				o++;
		}
		reg.rowvotes[y] = m >= o;
		rewritten += m < o;
	}
	for (x = 0; x < cols; x++) {
		long s = 0, m = 0, o = 0;
		int blank = 1;
		for (y = 0; y < height; y++) {
			int from = y + n;
			Line line = tviewline(reg.top + y);
			const Glyph *same, *src;

			if (blank && (!glyphblank(reg.snap + (size_t)y * cols + x) ||
			    (line && !glyphblank(&line[x]))))
				blank = 0;
			if (!line || from < 0 || from >= height)
				continue;	/* new row: no prediction */
			if (!reg.rowvotes[y])
				continue;	/* rewritten row */
			same = reg.snap + (size_t)y * cols + x;
			src = reg.snap + (size_t)from * cols + x;
			if (glypheq(same, src))
				continue;
			if (glypheq(&line[x], same))
				s++;
			else if (glypheq(&line[x], src))
				m++;
			else
				o++;
		}
		informative += s + m + o;
		reg.colinfo[x] = (s + m + o > 0 ? COL_VOTED : 0) |
		    (blank ? COL_BLANK : 0);
		if (m > s && m > o) {
			if (left < 0)
				left = x;
			right = x + 1;
		}
	}
	if (!informative && rewritten) {	/* every row was rewritten */
		reg.left = reg.right = 0;
		return 0;
	}
	if (!informative) {	/* uniform content: nothing to tell apart */
		reg.left = 0;
		reg.right = cols;
		return 1;
	}
	if (left < 0) {
		reg.left = reg.right = 0;
		return 0;
	}
	{
		int x0 = left, x1 = right, g0, g1;

		while (x0 > 0 && !(reg.colinfo[x0 - 1] & COL_VOTED))
			x0--;
		if (x0 == 0)
			left = 0;
		else if (widestgap(x0, left, &g0, &g1))
			left = g1;
		while (x1 < cols && !(reg.colinfo[x1] & COL_VOTED))
			x1++;
		if (x1 == cols)
			right = cols;
		else if (widestgap(right, x1, &g0, &g1))
			right = g0;
	}
	for (x = left; x < right; x++) {
		for (y = 0; y < height; y++) {
			int from = y + n;
			Line line = tviewline(reg.top + y);
			const Glyph *same, *src;

			if (!line || from < 0 || from >= height)
				continue;
			same = reg.snap + (size_t)y * cols + x;
			src = reg.snap + (size_t)from * cols + x;
			if (glypheq(same, src) || glypheq(&line[x], same))
				continue;
			if (glypheq(&line[x], src))
				moved++;
			else
				other++;
		}
	}
	reg.left = left;
	reg.right = right;
	return moved > other;
}

static void
scrollrows(int rows)
{
	if (rows > 0)
		kscrollup(&(Arg){ .i = rows });
	else if (rows < 0)
		kscrolldown(&(Arg){ .i = -rows });
}

/* Keep the precise offset within half a row by moving the logical view. */
static void
normalize(double now)
{
	ss_limit(&sst.ss, thistlen() - tscrolled(), tscrolled());
	scrollrows(ss_snap_rows(sst.ss.offset));
	follow(now);
}

static void
settle(double now)
{
	normalize(now);
	ss_settle(&sst.ss, now, sst.duration);
}

/* ST_SMOOTHSCROLL_LOG=file: one line per frame drawn while something moves
 * (time in ms, scrollback / region / app offsets in rows, rows app-mode
 * region scrolls moved so far), to check pacing and smoothness. */
static void
framelog(double now, int moving)
{
	static FILE *log;
	static int opened;

	if (!opened) {
		const char *path = getenv("ST_SMOOTHSCROLL_LOG");
		opened = 1;
		if (path && *path && (log = fopen(path, "w")))
			setvbuf(log, NULL, _IOLBF, 0);
	}
	if (log && (moving || sst.moved))
		fprintf(log, "%.3f %.4f %.4f %.4f %ld\n", now, sst.ss.offset,
		    reg.active ? reg.ss.offset : 0.0, app.offset, app.scrolled);
}

void
smoothscroll_frame(void)
{
	double now;
	int moving;

	/* Synchronized output (mode 2026) withholds frames: let its scrolls
	 * accumulate and start the glide when the batch is presented. */
	if (!sst.enabled || xismode(MODE_SYNC))
		return;
	now = nowms();
	follow(now);
	if (ss_settle_due(&sst.ss, now))
		settle(now);
	moving = ss_tick(&sst.ss, now);
	if (reg.active) {
		/* The batch is presented now: a band the app mostly rewrote
		 * does not glide.  Later frames only refine the moving columns
		 * (an app may repaint its panels after the scroll is shown). */
		int scrolled = smoothscroll_classify();
		if (reg.fresh) {
			reg.ss.start = now;
			reg.fresh = 0;
			if (!scrolled)
				regreset();
		}
		if (reg.active && ss_tick(&reg.ss, now))
			moving = 1;
		else if (reg.active)
			regreset();
	}
	if (app.on && (app.rows != trow() || app.cols != tcol() ||
	    app.alt != tisaltscreen()))
		appclear();	/* resize / screen switch: the app starts over */
	if (app.on) {
		appoffset(now);
		if ((app.offset != 0.0 || app.nterms) && app.nrects)
			moving = 1;
	}
	framelog(now, moving);
	/* The first frame back at rest also redraws everything so no row keeps
	 * a stale shifted copy in retained (damage-tracked) buffers. */
	sst.full = moving || sst.moved;
	sst.moved = moving;
	if (sst.full)
		tfulldirt();
}

void
smoothscroll_rows(int *first, int *end)
{
	int last;

	if (!sst.enabled || sst.ss.offset == 0.0) {
		*first = 0;
		*end = trow();
		return;
	}
	ss_visible_rows(sst.ss.offset, trow(), first, &last);
	*end = last + 1;
}

static void
drawghost(Glyph *line, int asrow, double shift)
{
	if (reg.right <= reg.left)
		return;
	ghost.on = 1;
	ghost.shift = shift;
	xdrawline(line, reg.left, asrow, reg.right);
	ghost.on = 0;
}

void
smoothscroll_drawextra(void)
{
	int y, k, first, end, cols = tcol();
	double o;
	Line line;

	if (!sst.enabled)
		return;
	if (sst.ss.offset != 0.0) {
		smoothscroll_rows(&first, &end);
		for (y = first; y < end; y++) {
			if (y == 0)
				y = trow();	/* drawregion() drew the grid */
			if (y < end && (line = tviewline(y)))
				xdrawline(line, 0, y, cols);
		}
		return;
	}
	appdrawextra();
	if (!reg.active || reg.ss.offset == 0.0)
		return;
	/* Copy k above the band is drawn at row top - k + o; below at bot + k + o. */
	o = reg.ss.offset;
	for (k = 1; k <= reg.nabove && k < o + 1; k++)
		drawghost(reg.above[k - 1], reg.top, o - k);
	for (k = 1; k <= reg.nbelow && k + o < 1; k++)
		drawghost(reg.below[k - 1], reg.bot, o + k);
}

static int
regmoves(int x, int y)
{
	return reg.active && y >= reg.top && y <= reg.bot &&
	    x >= reg.left && x < reg.right;
}

double
smoothscroll_shift(int x, int y)
{
	if (!sst.enabled)
		return 0.0;
	if (ghost.on)
		return ghost.shift;
	if (sst.ss.offset != 0.0)
		return sst.ss.offset;
	if (app.offset != 0.0 && apprect(x, y))
		return app.offset;
	if (regmoves(x, y))
		return reg.ss.offset;
	return 0.0;
}

int
smoothscroll_clip(int x, int y, int *top, int *end, int *left, int *right)
{
	if (!sst.enabled)
		return 0;
	if (sst.ss.offset != 0.0) {
		*top = 0;
		*end = trow();
		*left = 0;
		*right = tcol();
		return 1;
	}
	if (ghost.app) {
		*top = ghost.top;
		*end = ghost.end;
		*left = ghost.left;
		*right = ghost.right;
		return 1;
	}
	{
		const AppRect *r = ghost.on ? NULL : apprect(x, y);
		if (r) {
			*top = r->top;
			*end = r->bot + 1;
			*left = r->left;
			*right = r->right;
			return 1;
		}
		if (!ghost.on && imageline &&
		    smoothscroll_copy_row(imageline) != INT_MIN) {
			*top = *end = *left = *right = 0;	/* nowhere */
			return 1;
		}
	}
	if (ghost.on || regmoves(x, y)) {
		*top = reg.top;
		*end = reg.bot + 1;
		*left = reg.left;
		*right = reg.right;
		return 1;
	}
	return 0;
}

int
smoothscroll_span(int y, int *left, int *right)
{
	if (!sst.enabled || ghost.on || !reg.active || y < reg.top ||
	    y > reg.bot || (reg.left == 0 && reg.right == reg.cols))
		return 0;
	*left = reg.left;
	*right = reg.right;
	return 1;
}

/* Grow `end` down to `edge` when it splits the run starting at x. */
static int
cut(int x, int end, int edge)
{
	return edge > x && edge < end ? edge : end;
}

int
smoothscroll_segment(int y, int x, int x2)
{
	int left, right, i, end = x2;

	if (!sst.enabled || ghost.on)
		return x2;
	if (smoothscroll_span(y, &left, &right)) {
		end = cut(x, end, left);
		end = cut(x, end, right);
	}
	if (app.on)
		for (i = 0; i < app.nrects; i++) {
			const AppRect *r = &app.rect[i];
			if (y < r->top || y > r->bot)
				continue;
			end = cut(x, end, r->left);
			end = cut(x, end, r->right);
		}
	return end;
}

int
smoothscroll_band(int *top, int *end)
{
	if (!sst.enabled)
		return 0;
	if (sst.ss.offset != 0.0) {
		*top = 0;
		*end = trow();
		return 1;
	}
	if (reg.active && reg.ss.offset != 0.0) {
		*top = reg.top;
		*end = reg.bot + 1;
		return 1;
	}
	if (app.on && app.offset != 0.0 && app.nrects) {
		int i;
		*top = trow();
		*end = 0;
		for (i = 0; i < app.nrects; i++) {
			*top = MIN(*top, app.rect[i].top);
			*end = MAX(*end, app.rect[i].bot + 1);
		}
		return 1;
	}
	return 0;
}

int
smoothscroll_plain(void)
{
	return ghost.on;
}

double
smoothscroll_view_shift(void)
{
	return sst.enabled ? sst.ss.offset : 0.0;
}

int
smoothscroll_needs_full(void)
{
	return sst.enabled && sst.full;
}

int
smoothscroll_pending(void)
{
	return smoothscroll_timeout() >= 0;
}

double
smoothscroll_timeout(void)
{
	if (!sst.enabled)
		return -1;
	if (reg.active || (app.on && app.nterms))
		return 0.0;
	return ss_wait(&sst.ss, nowms());
}

int
smoothscroll_pixels(double pixels, double cellh, int held)
{
	double now;

	if (!sst.enabled || tisaltscreen() || !(cellh > 0))
		return 0;
	now = nowms();
	regreset();
	follow(now);
	ss_precise(&sst.ss, pixels / cellh, now, held, SS_SETTLE_MS);
	normalize(now);
	return 1;
}

void
smoothscroll_release(void)
{
	if (!sst.enabled || sst.ss.state != SS_PRECISE)
		return;
	sst.ss.held = 0;
	sst.ss.settle_at = nowms() + SS_SETTLE_MS;
}
