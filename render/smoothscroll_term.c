/* See LICENSE for license details. */
/* Smooth-scroll terminal glue: watches the logical scroll position, drives
 * the engine in smoothscroll.c and draws the partial rows the offset exposes.
 * Renderers only ask for the pixel offset; see reference/smooth-scroll.md. */

#include <stdint.h>
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
	int enabled;
	double duration;
	SmoothScroll ss;
	/* Logical view at the previous sync. */
	int valid, alt, rows, cols, scr;
	Line top;
	/* The previous frame was drawn with a nonzero offset. */
	int moved, full;
} sst;

static double
monotonicms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1E3 + ts.tv_nsec / 1E6;
}

/* Replaced by the unit tests to step time deterministically. */
static double (*nowms)(void) = monotonicms;

void
smoothscroll_configure(int enabled, double duration_ms)
{
	sst.enabled = enabled;
	sst.duration = duration_ms > 0 ? duration_ms : 0;
	sst.valid = 0;
	ss_reset(&sst.ss);
}

int
smoothscroll_enabled(void)
{
	return sst.enabled;
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

	/* Full-screen applications scroll themselves: no pixel offsets there. */
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

	if (!sst.enabled)
		return;
	now = nowms();
	follow(now);
	if (ss_settle_due(&sst.ss, now))
		settle(now);
	moving = ss_tick(&sst.ss, now);
	/* The first frame back at rest also redraws everything so no row keeps
	 * a stale translated copy in retained (damage-tracked) buffers. */
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

void
smoothscroll_drawextra(void)
{
	int y, first, end, cols = tcol();
	Line line;

	if (!sst.enabled || sst.ss.offset == 0.0)
		return;
	smoothscroll_rows(&first, &end);
	for (y = first; y < end; y++) {
		if (y == 0)
			y = trow();	/* drawregion() already drew the grid */
		if (y < end && (line = tviewline(y)))
			xdrawline(line, 0, y, cols);
	}
}

double
smoothscroll_offset_px(double cellh, double scale)
{
	if (!sst.enabled || sst.ss.offset == 0.0)
		return 0.0;
	return ss_device_round(sst.ss.offset * cellh, scale);
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
	return ss_wait(&sst.ss, nowms());
}

int
smoothscroll_pixels(double pixels, double cellh, int held)
{
	double now;

	if (!sst.enabled || tisaltscreen() || !(cellh > 0))
		return 0;
	now = nowms();
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
