/* See LICENSE for license details. */
/* Pure smooth-scroll engine: offset math and the animation state machine.
 * Terminal and renderer integration lives in smoothscroll_term.c. */

#include <math.h>
#include <string.h>

#include "smoothscroll.h"

/* Fraction of the starting offset still remaining at `progress` (0..1).
 * Cubic ease-out: fast departure, gentle arrival, exactly 0 at the end. */
double
ss_ease(double progress)
{
	double rest;

	if (!(progress > 0.0))
		return 1.0;
	if (progress >= 1.0)
		return 0.0;
	rest = 1.0 - progress;
	return rest * rest * rest;
}

/* `above` rows of history exist above row 0 and `below` rows of the live
 * screen exist below the last row; never reveal anything past either end. */
double
ss_clamp(double offset, int above, int below)
{
	if (above < 0)
		above = 0;
	if (below < 0)
		below = 0;
	if (offset > above)
		return above;
	if (offset < -below)
		return -below;
	return offset;
}

/* Rows that intersect the viewport once row y is drawn at y + offset. */
void
ss_visible_rows(double offset, int rows, int *first, int *last)
{
	*first = (int)floor(-offset);
	*last = (int)ceil(rows - offset) - 1;
	if (*first > 0)
		*first = 0;
	if (*last < rows - 1)
		*last = rows - 1;
}

/* Whole rows to scroll so the remaining offset is within half a row. */
int
ss_snap_rows(double offset)
{
	return (int)lround(offset);
}

/* Snap a pixel offset to the device pixel grid so glyphs stay crisp. */
double
ss_device_round(double pixels, double scale)
{
	if (!(scale > 0.0))
		scale = 1.0;
	return floor(pixels * scale + 0.5) / scale;
}

/* Grid row under window coordinate y while the grid is drawn offsetpx lower.
 * The result may lie outside the grid; callers clamp as they always did. */
int
ss_row_at(double y, double top, double cellh, double offsetpx)
{
	if (!(cellh > 0.0))
		return 0;
	return (int)floor((y - top - offsetpx) / cellh);
}

int
ss_render_path(int enabled, int gpu_active)
{
	if (!enabled)
		return SS_RENDER_OFF;
	return gpu_active ? SS_RENDER_GPU : SS_RENDER_CPU;
}

void
ss_reset(SmoothScroll *ss)
{
	memset(ss, 0, sizeof(*ss));
}

static double
ss_value(const SmoothScroll *ss, double now)
{
	if (ss->state != SS_ANIMATING)
		return ss->offset;
	if (!(ss->duration > 0.0))
		return 0.0;
	return ss->from * ss_ease((now - ss->start) / ss->duration);
}

static void
ss_animate(SmoothScroll *ss, double offset, double now, double duration)
{
	if (offset == 0.0 || !(duration > 0.0)) {
		ss->state = SS_IDLE;
		ss->offset = ss->from = 0.0;
		return;
	}
	ss->state = SS_ANIMATING;
	ss->offset = ss->from = offset;
	ss->start = now;
	ss->duration = duration;
}

/* The logical view moved: add `rows` to the visual offset so content stays
 * where it was drawn, then glide to rest (unless precise input owns it). */
void
ss_shift(SmoothScroll *ss, double rows, double now, double duration)
{
	double offset = ss_value(ss, now) + rows;

	if (ss->state == SS_PRECISE) {
		ss->offset = offset;
		return;
	}
	ss_animate(ss, offset, now, duration);
}

/* Bound the current offset and any animation still in flight. */
void
ss_limit(SmoothScroll *ss, int above, int below)
{
	ss->offset = ss_clamp(ss->offset, above, below);
	if (ss->state == SS_ANIMATING) {
		ss->from = ss_clamp(ss->from, above, below);
		if (ss->from == 0.0) {
			ss->state = SS_IDLE;
			ss->offset = 0.0;
		}
	}
}

/* Precise input moves the content by exactly `rows`; an animation in flight
 * is taken over from wherever it currently is. */
void
ss_precise(SmoothScroll *ss, double rows, double now, int held, double settle)
{
	ss->offset = ss_value(ss, now) + rows;
	ss->state = SS_PRECISE;
	ss->held = held;
	ss->settle_at = now + settle;
}

int
ss_settle_due(const SmoothScroll *ss, double now)
{
	return ss->state == SS_PRECISE && !ss->held && now >= ss->settle_at;
}

/* Glide the sub-row remainder to rest.  Callers first scroll the logical view
 * by ss_snap_rows() so the remainder is at most half a row. */
void
ss_settle(SmoothScroll *ss, double now, double duration)
{
	ss->held = 0;
	ss_animate(ss, ss->offset, now, duration);
}

/* Advance to `now`.  Returns 1 while the offset is nonzero. */
int
ss_tick(SmoothScroll *ss, double now)
{
	if (ss->state == SS_ANIMATING) {
		ss->offset = ss_value(ss, now);
		if ((now - ss->start) >= ss->duration || ss->offset == 0.0) {
			ss->state = SS_IDLE;
			ss->offset = ss->from = 0.0;
		}
	}
	return ss->offset != 0.0;
}

/* Milliseconds until another frame is due (0: draw now; frontends pace
 * animation frames themselves), or -1 when input-driven or idle. */
double
ss_wait(const SmoothScroll *ss, double now)
{
	if (ss->state == SS_ANIMATING)
		return 0.0;
	if (ss->state == SS_PRECISE && !ss->held)
		return ss->settle_at > now ? ss->settle_at - now : 0.0;
	return -1.0;
}
