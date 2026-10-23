/* See LICENSE for license details. */
/* Smooth-scroll terminal glue: drives the engine in smoothscroll.c for two
 * kinds of motion and tells renderers, per grid row, how far to shift it and
 * what to clip it to.  See reference/smooth-scroll.md.
 *
 * - Scrollback: follows term.scr; every row shifts, clipped to the grid.
 * - Regions: an application scrolled a DECSTBM region (SU/SD, IND/RI at a
 *   margin, IL/DL at the top margin); only rows of that band shift, clipped
 *   to it.  The lines that left the band are kept as copies and slide out. */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <wchar.h>

#include "../st.h"
#include "../win.h"
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
} reg;

/* A copied line being drawn outside the grid: its shift relative to the
 * band edge row it is drawn as, and no highlights (it is not on the grid). */
static struct {
	int on;
	double shift;
} ghost;

static double
monotonicms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1E3 + ts.tv_nsec / 1E6;
}

/* Replaced by the unit tests to step time deterministically. */
static double (*nowms)(void) = monotonicms;

static void
ghostsfree(Glyph **list, int *n, int from)
{
	int i;

	for (i = from; i < *n; i++)
		free(list[i]);
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
	memset(&reg, 0, sizeof(reg));
}

void
smoothscroll_configure(int enabled, double duration_ms, int regions)
{
	regreset();
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
ghostspush(Glyph **list, int *count, int drop, int y0, int step, int n)
{
	int i, keep;

	drop = MIN(drop, *count);
	for (i = 0; i < drop; i++)
		free(list[i]);
	memmove(list, list + drop, (*count - drop) * sizeof(*list));
	*count -= drop;
	keep = MIN(*count, reg.cap - n);
	ghostsfree(list, count, MAX(0, keep));
	memmove(list + n, list, *count * sizeof(*list));
	for (i = 0; i < n; i++)
		list[i] = copyline(y0 + i * step, reg.cols);
	*count += n;
}

/* Called by tscrollup()/tscrolldown() before rows [orig, bot] of the region
 * [top, bot] move by n (positive: content moves up). */
void
smoothscroll_region(int top, int bot, int orig, int n)
{
	int alt, height, distance;
	double now;

	if (!sst.enabled || !(sst.regions || sst.decsclm) || !n)
		return;
	alt = tisaltscreen();
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
		reg.above = calloc(height, sizeof(*reg.above));
		reg.below = calloc(height, sizeof(*reg.below));
		if (!reg.above || !reg.below) {
			regreset();
			return;
		}
	}
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
		if (reg.fresh) {
			reg.ss.start = now;
			reg.fresh = 0;
		}
		if (ss_tick(&reg.ss, now))
			moving = 1;
		else
			regreset();
	}
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
	ghost.on = 1;
	ghost.shift = shift;
	xdrawline(line, 0, asrow, reg.cols);
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
	if (!reg.active || reg.ss.offset == 0.0)
		return;
	/* Copy k above the band is drawn at row top - k + o; below at bot + k + o. */
	o = reg.ss.offset;
	for (k = 1; k <= reg.nabove && k < o + 1; k++)
		drawghost(reg.above[k - 1], reg.top, o - k);
	for (k = 1; k <= reg.nbelow && k + o < 1; k++)
		drawghost(reg.below[k - 1], reg.bot, o + k);
}

double
smoothscroll_row_shift(int y)
{
	if (!sst.enabled)
		return 0.0;
	if (ghost.on)
		return ghost.shift;
	if (sst.ss.offset != 0.0)
		return sst.ss.offset;
	if (reg.active && y >= reg.top && y <= reg.bot)
		return reg.ss.offset;
	return 0.0;
}

int
smoothscroll_row_clip(int y, int *top, int *end)
{
	if (!sst.enabled)
		return 0;
	if (sst.ss.offset != 0.0) {
		*top = 0;
		*end = trow();
		return 1;
	}
	if (reg.active && (ghost.on || (y >= reg.top && y <= reg.bot))) {
		*top = reg.top;
		*end = reg.bot + 1;
		return 1;
	}
	return 0;
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
	if (reg.active)
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
