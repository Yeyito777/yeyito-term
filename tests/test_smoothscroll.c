/* See LICENSE for license details. */
/* Smooth scrolling: engine math/state machine and terminal glue against a
 * fake scrollback buffer with a stepped clock (no real time, never hangs). */

#include <math.h>
#include <string.h>

#include "test.h"
#include "../render/smoothscroll.c"
#include "../render/smoothscroll_term.c"

#define NEAR(a, b) (fabs((a) - (b)) < 1e-9)

/* ---- Fake terminal ---- */

#define FAKE_HIST 100
#define FAKE_ROWS 10
#define FAKE_COLS 12
#define FAKE_LINES (FAKE_HIST + FAKE_ROWS)

static Glyph store[FAKE_LINES][FAKE_COLS], altstore[FAKE_ROWS][FAKE_COLS];
static Line lines[FAKE_LINES];
static int fhistn, fscr, frows, falt, fulldirts, ndrawn, drawn[64];
static unsigned int fmode;
static double fakenow;
/* What each xdrawline() call drew: first rune, shift, departing copy? */
static int drawnid[64], drawnplain[64], drawnx1[64], drawnx2[64];
static double drawnshift[64];
static int nextid;

int xismode(unsigned int flags) { return (fmode & flags) != 0; }

static double fakeclock(void) { return fakenow; }

int trow(void) { return frows; }
int tcol(void) { return FAKE_COLS; }
int tisaltscreen(void) { return falt; }
int tscrolled(void) { return fscr; }
int thistlen(void) { return fhistn; }
void tfulldirt(void) { fulldirts++; }

Line
tviewline(int y)
{
	int i = fhistn - fscr + y;

	if (falt)
		return y >= 0 && y < frows ? altstore[y] : NULL;
	if (i < 0 || i >= fhistn + frows)
		return NULL;
	return lines[i];
}

void
kscrollup(const Arg *a)
{
	fscr += MIN(a->i, fhistn - fscr);
}

void
kscrolldown(const Arg *a)
{
	fscr -= MIN(a->i, fscr);
}

void
xdrawline(Line line, int x1, int y, int x2)
{
	if (ndrawn < (int)(sizeof(drawn) / sizeof(drawn[0]))) {
		drawnid[ndrawn] = line[x1].u;
		drawnx1[ndrawn] = x1;
		drawnx2[ndrawn] = x2;
		drawnshift[ndrawn] = smoothscroll_shift(x1, y);
		drawnplain[ndrawn] = smoothscroll_plain();
		drawn[ndrawn++] = y;
	}
}

/* Scrollback shift of the view in pixels for 20 px rows. */
static double
viewpx(void)
{
	return ss_device_round(smoothscroll_shift(0, 0) * 20, 1);
}

static void
fillrow(int y, int id)
{
	for (int x = 0; x < FAKE_COLS; x++)
		altstore[y][x] = (Glyph){ .u = id };
}

/* Apply a region scroll to the alternate screen as st does: the hook runs
 * first, then rows move and the vacated rows get fresh content. */
static void
region_scroll(int top, int bot, int orig, int n)
{
	int y;

	smoothscroll_region(top, bot, orig, n);
	if (n > 0) {
		for (y = orig; y <= bot - n; y++)
			memcpy(altstore[y], altstore[y + n], sizeof(altstore[y]));
		for (y = bot - n + 1; y <= bot; y++)
			fillrow(y, nextid++);
	} else {
		for (y = bot; y >= orig - n; y--)
			memcpy(altstore[y], altstore[y + n], sizeof(altstore[y]));
		for (y = orig; y < orig - n; y++)
			fillrow(y, nextid++);
	}
}

static void fake_reset(int enabled);
static void frame_at(double t);

static void
alt_reset(int regions)
{
	int y;

	fake_reset(1);
	smoothscroll_configure(1, 100, regions);
	falt = 1;
	fmode = 0;
	for (y = 0; y < FAKE_ROWS; y++)
		fillrow(y, 100 + y);	/* row ids in every column */
	nextid = 200;
	frame_at(1000);
	fulldirts = ndrawn = 0;
}

/* One line of output: the oldest line is recycled as the new bottom line. */
static void
output_line(void)
{
	Line oldest = lines[0];

	memmove(lines, lines + 1, (FAKE_LINES - 1) * sizeof(*lines));
	lines[FAKE_LINES - 1] = oldest;
	if (fscr > 0)
		fscr++;	/* st keeps a scrolled view on the same lines */
}

static void
fake_reset(int enabled)
{
	int i;

	for (i = 0; i < FAKE_LINES; i++)
		lines[i] = store[i];
	fhistn = FAKE_HIST;
	fscr = 0;
	frows = FAKE_ROWS;
	falt = 0;
	fulldirts = ndrawn = 0;
	fakenow = 1000;
	nowms = fakeclock;
	fmode = 0;
	smoothscroll_configure(enabled, 100, 1);
	smoothscroll_frame();	/* first sync records the view */
	fulldirts = 0;
}

static void
frame_at(double t)
{
	fakenow = t;
	smoothscroll_frame();
}

/* ---- Engine ---- */

TEST(ease_runs_from_one_to_exactly_zero)
{
	ASSERT(NEAR(1.0, ss_ease(0.0)));
	ASSERT(NEAR(0.125, ss_ease(0.5)));
	ASSERT(NEAR(0.0, ss_ease(1.0)));
	ASSERT(NEAR(0.0, ss_ease(7.0)));
	ASSERT(ss_ease(0.2) > ss_ease(0.3));
	ASSERT(NEAR(1.0, ss_ease(-1.0)));
}

TEST(clamp_never_reveals_past_buffer_ends)
{
	ASSERT(NEAR(0.0, ss_clamp(-2.5, 10, 0)));	/* at bottom */
	ASSERT(NEAR(0.0, ss_clamp(2.5, 0, 10)));	/* at top of history */
	ASSERT(NEAR(-3.0, ss_clamp(-7.0, 10, 3)));
	ASSERT(NEAR(4.0, ss_clamp(9.0, 4, 3)));
	ASSERT(NEAR(1.25, ss_clamp(1.25, 4, 3)));
}

TEST(visible_rows_include_partial_rows)
{
	int first, last;

	ss_visible_rows(0.0, 24, &first, &last);
	ASSERT_EQ(0, first);
	ASSERT_EQ(23, last);
	ss_visible_rows(0.25, 24, &first, &last);
	ASSERT_EQ(-1, first);
	ASSERT_EQ(23, last);
	ss_visible_rows(-0.25, 24, &first, &last);
	ASSERT_EQ(0, first);
	ASSERT_EQ(24, last);
	ss_visible_rows(2.5, 24, &first, &last);
	ASSERT_EQ(-3, first);
	ASSERT_EQ(23, last);
	ss_visible_rows(-3.0, 24, &first, &last);
	ASSERT_EQ(0, first);
	ASSERT_EQ(26, last);
}

TEST(snap_rounds_to_nearest_row)
{
	ASSERT_EQ(0, ss_snap_rows(0.49));
	ASSERT_EQ(1, ss_snap_rows(0.5));
	ASSERT_EQ(-1, ss_snap_rows(-0.5));
	ASSERT_EQ(0, ss_snap_rows(-0.49));
	ASSERT_EQ(3, ss_snap_rows(2.7));
}

TEST(device_round_snaps_to_physical_pixels)
{
	ASSERT(NEAR(10.0, ss_device_round(10.4, 1.0)));
	ASSERT(NEAR(-10.0, ss_device_round(-10.4, 1.0)));
	ASSERT(NEAR(10.5, ss_device_round(10.3, 2.0)));
	ASSERT(NEAR(10.0, ss_device_round(10.2, 2.0)));
	ASSERT(NEAR(3.0, ss_device_round(3.2, 0.0)));
}

TEST(row_at_follows_the_drawn_grid)
{
	/* border 2, 20 px rows, grid drawn 10 px lower */
	ASSERT_EQ(0, ss_row_at(2 + 10 + 5, 2, 20, 10));
	ASSERT_EQ(-1, ss_row_at(2 + 5, 2, 20, 10));
	ASSERT_EQ(1, ss_row_at(2 + 25, 2, 20, 0));
	ASSERT_EQ(2, ss_row_at(2 + 25, 2, 20, -20));
}

TEST(render_path_falls_back_to_cpu)
{
	ASSERT_EQ(SS_RENDER_OFF, ss_render_path(0, 1));
	ASSERT_EQ(SS_RENDER_OFF, ss_render_path(0, 0));
	ASSERT_EQ(SS_RENDER_GPU, ss_render_path(1, 1));
	ASSERT_EQ(SS_RENDER_CPU, ss_render_path(1, 0));
}

TEST(shift_glides_to_exact_rest)
{
	SmoothScroll ss;

	ss_reset(&ss);
	ss_shift(&ss, -3, 0, 100);
	ASSERT_EQ(SS_ANIMATING, ss.state);
	ASSERT(ss_tick(&ss, 50));
	ASSERT(ss.offset < 0 && ss.offset > -3);
	ASSERT(NEAR(-3 * 0.125, ss.offset));
	ASSERT(!ss_tick(&ss, 100));
	ASSERT_EQ(SS_IDLE, ss.state);
	ASSERT(ss.offset == 0.0);
	ASSERT(NEAR(-1.0, ss_wait(&ss, 100)));
}

TEST(shift_retargets_from_current_position)
{
	SmoothScroll ss;

	ss_reset(&ss);
	ss_shift(&ss, -3, 0, 100);
	ss_shift(&ss, -3, 50, 100);
	ASSERT(NEAR(-3 * 0.125 - 3, ss.offset));
	ASSERT(NEAR(50, ss.start));
	ASSERT(ss_tick(&ss, 140));
	ASSERT(!ss_tick(&ss, 150));
}

TEST(zero_duration_never_animates)
{
	SmoothScroll ss;

	ss_reset(&ss);
	ss_shift(&ss, -3, 0, 0);
	ASSERT_EQ(SS_IDLE, ss.state);
	ASSERT(ss.offset == 0.0);
}

TEST(limit_bounds_animation_in_flight)
{
	SmoothScroll ss;

	ss_reset(&ss);
	ss_shift(&ss, 5, 0, 100);
	ss_limit(&ss, 2, 0);
	ASSERT(NEAR(2.0, ss.from));
	ss_limit(&ss, 0, 0);
	ASSERT_EQ(SS_IDLE, ss.state);
	ASSERT(ss.offset == 0.0);
}

TEST(precise_waits_for_release_then_settles)
{
	SmoothScroll ss;

	ss_reset(&ss);
	ss_precise(&ss, 0.3, 0, 1, 60);
	ASSERT_EQ(SS_PRECISE, ss.state);
	ASSERT(!ss_settle_due(&ss, 10000));
	ASSERT(NEAR(-1.0, ss_wait(&ss, 0)));	/* held: input-driven */
	ss_precise(&ss, 0.1, 10, 0, 60);
	ASSERT(NEAR(0.4, ss.offset));
	ASSERT(!ss_settle_due(&ss, 69));
	ASSERT(NEAR(60.0, ss_wait(&ss, 10)));
	ASSERT(ss_settle_due(&ss, 70));
	ss_settle(&ss, 70, 100);
	ASSERT_EQ(SS_ANIMATING, ss.state);
	ASSERT(!ss_tick(&ss, 170));
	ASSERT(ss.offset == 0.0);
}

TEST(precise_takes_over_animation)
{
	SmoothScroll ss;

	ss_reset(&ss);
	ss_shift(&ss, -2, 0, 100);
	ss_precise(&ss, 0.5, 50, 1, 60);
	ASSERT_EQ(SS_PRECISE, ss.state);
	ASSERT(NEAR(-2 * 0.125 + 0.5, ss.offset));
	ss_shift(&ss, -1, 60, 100);	/* compensation while precise */
	ASSERT_EQ(SS_PRECISE, ss.state);
	ASSERT(NEAR(-2 * 0.125 - 0.5, ss.offset));
}

/* ---- Terminal glue ---- */

TEST(disabled_is_inert)
{
	fake_reset(0);
	kscrollup(&(Arg){ .i = 3 });
	frame_at(1000);
	ASSERT_EQ(0, fulldirts);
	ASSERT(viewpx() == 0.0);
	ASSERT(!smoothscroll_needs_full());
	ASSERT(!smoothscroll_pending());
	ASSERT_EQ(0, smoothscroll_pixels(30, 20, 1));
	ASSERT_EQ(3, fscr);	/* pixels did not scroll */
	smoothscroll_drawextra();
	ASSERT_EQ(0, ndrawn);
}

TEST(discrete_scroll_up_glides_and_exposes_rows_below)
{
	int first, end;

	fake_reset(1);
	kscrollup(&(Arg){ .i = 3 });
	frame_at(1000);
	/* old content stays put: grid drawn three rows higher */
	ASSERT(NEAR(-60.0, viewpx()));
	ASSERT(smoothscroll_needs_full());
	ASSERT(fulldirts > 0);
	smoothscroll_rows(&first, &end);
	ASSERT_EQ(0, first);
	ASSERT_EQ(13, end);
	smoothscroll_drawextra();
	ASSERT_EQ(3, ndrawn);
	ASSERT_EQ(10, drawn[0]);
	ASSERT_EQ(12, drawn[2]);
	ASSERT(smoothscroll_pending());
	ASSERT(smoothscroll_timeout() == 0.0);	/* due now; frontend paces */

	frame_at(1050);
	ASSERT(NEAR(ss_device_round(-60.0 * 0.125, 1), viewpx()));
	frame_at(1100);
	ASSERT(viewpx() == 0.0);
	ASSERT(smoothscroll_needs_full());	/* one clean frame at rest */
	ASSERT(!smoothscroll_pending());
	frame_at(1116);
	ASSERT(!smoothscroll_needs_full());
	ASSERT(smoothscroll_timeout() < 0);	/* idle: no frames scheduled */
	ASSERT_EQ(3, fscr);
}

TEST(discrete_scroll_down_exposes_rows_above)
{
	fake_reset(1);
	kscrollup(&(Arg){ .i = 5 });
	frame_at(1000);
	frame_at(2000);
	ndrawn = 0;
	kscrolldown(&(Arg){ .i = 2 });
	frame_at(2000);
	ASSERT(NEAR(40.0, viewpx()));
	smoothscroll_drawextra();
	ASSERT_EQ(2, ndrawn);
	ASSERT_EQ(-2, drawn[0]);
	ASSERT_EQ(-1, drawn[1]);
}

TEST(output_never_animates)
{
	fake_reset(1);
	output_line();	/* at the bottom: content scrolls as always */
	frame_at(1000);
	ASSERT(viewpx() == 0.0);
	kscrollup(&(Arg){ .i = 4 });
	frame_at(1000);
	frame_at(1200);
	output_line();	/* scrolled up: view keeps its lines, scr grows */
	output_line();
	frame_at(1300);
	ASSERT_EQ(6, fscr);
	ASSERT(viewpx() == 0.0);
	ASSERT(!smoothscroll_pending());
}

TEST(alt_screen_keeps_application_scrolling)
{
	fake_reset(1);
	falt = 1;
	frame_at(1000);
	kscrollup(&(Arg){ .i = 3 });
	frame_at(1000);
	ASSERT(viewpx() == 0.0);
	ASSERT_EQ(0, smoothscroll_pixels(30, 20, 1));
	ASSERT(!smoothscroll_pending());
	/* Leaving the alternate screen never animates the switch. */
	falt = 0;
	frame_at(1010);
	ASSERT(viewpx() == 0.0);
}

TEST(big_jump_glides_from_one_screen)
{
	fake_reset(1);
	kscrollup(&(Arg){ .i = 80 });
	frame_at(1000);
	ASSERT(NEAR(-200.0, viewpx()));
	frame_at(1100);
	kscrolldown(&(Arg){ .i = 80 });
	frame_at(1200);
	ASSERT(NEAR(200.0, viewpx()));
}

TEST(pixels_follow_input_and_settle_on_a_row)
{
	fake_reset(1);
	ASSERT_EQ(1, smoothscroll_pixels(30, 20, 1));	/* 1.5 rows up */
	ASSERT_EQ(2, fscr);
	ASSERT(NEAR(-10.0, viewpx()));
	frame_at(5000);	/* fingers still down: never snaps */
	ASSERT(NEAR(-10.0, viewpx()));
	ASSERT(!smoothscroll_pending());
	smoothscroll_release();
	ASSERT(smoothscroll_pending());
	frame_at(5030);	/* momentum window still open */
	ASSERT(NEAR(-10.0, viewpx()));
	frame_at(5061);	/* settles: snap to the nearest row, glide there */
	ASSERT(smoothscroll_pending());
	frame_at(5200);
	ASSERT(viewpx() == 0.0);
	ASSERT(fscr == 1 || fscr == 2);
	ASSERT(!smoothscroll_pending());
}

TEST(pixels_stop_at_buffer_ends)
{
	fake_reset(1);
	ASSERT_EQ(1, smoothscroll_pixels(-30, 20, 1));	/* below the bottom */
	ASSERT_EQ(0, fscr);
	ASSERT(viewpx() == 0.0);
	ASSERT_EQ(1, smoothscroll_pixels(20 * 500, 20, 1));	/* past the top */
	ASSERT_EQ(FAKE_HIST, fscr);
	ASSERT(viewpx() == 0.0);
}

TEST(resize_resets_offset)
{
	fake_reset(1);
	kscrollup(&(Arg){ .i = 3 });
	frame_at(1000);
	ASSERT(viewpx() != 0.0);
	frows = 8;
	frame_at(1010);
	ASSERT(viewpx() == 0.0);
}

/* ---- Application scroll regions ---- */

TEST(region_heuristics)
{
	/* alt, rows, top, bot, orig, n */
	ASSERT(ss_region_animates(1, 24, 0, 21, 0, 1));	/* nvim C-e: DL at top */
	ASSERT(ss_region_animates(1, 24, 0, 21, 0, -11));	/* C-u: IL at top */
	ASSERT(ss_region_animates(1, 24, 0, 23, 0, 3));	/* full alt screen */
	ASSERT(!ss_region_animates(1, 24, 0, 21, 5, 1));	/* DL mid-region: edit */
	ASSERT(!ss_region_animates(1, 24, 22, 23, 22, 1));	/* 2-row region */
	ASSERT(!ss_region_animates(1, 24, 0, 21, 0, 22));	/* whole region */
	ASSERT(!ss_region_animates(1, 24, 0, 21, 0, 0));
	ASSERT(!ss_region_animates(0, 24, 0, 23, 0, 1));	/* shell output */
	ASSERT(ss_region_animates(0, 24, 2, 20, 2, 1));	/* primary, partial */
}

TEST(region_scroll_glides_band_only)
{
	int top, end, left, right;

	alt_reset(1);
	region_scroll(0, 7, 0, 1);	/* rows 8-9: status line, cmdline */
	frame_at(1000);
	ASSERT(NEAR(1.0, smoothscroll_shift(0, 3)));	/* drawn where it was */
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 8)));
	ASSERT(smoothscroll_clip(0, 3, &top, &end, &left, &right));
	ASSERT_EQ(0, top);
	ASSERT_EQ(8, end);
	ASSERT(!smoothscroll_clip(0, 9, &top, &end, &left, &right));
	ASSERT(smoothscroll_band(&top, &end));
	ASSERT(smoothscroll_needs_full());
	ASSERT(fulldirts > 0);
	ASSERT(smoothscroll_timeout() == 0.0);
	/* The departing top line slides out above the band, as a plain copy. */
	smoothscroll_drawextra();
	ASSERT_EQ(1, ndrawn);
	ASSERT_EQ(0, drawn[0]);
	ASSERT_EQ(100, drawnid[0]);
	ASSERT(drawnplain[0]);
	ASSERT(NEAR(0.0, drawnshift[0]));
	ASSERT(!smoothscroll_plain());
	frame_at(1050);
	ASSERT(NEAR(0.125, smoothscroll_shift(0, 3)));
	ndrawn = 0;
	smoothscroll_drawextra();
	ASSERT_EQ(1, ndrawn);
	ASSERT(NEAR(-0.875, drawnshift[0]));
	frame_at(1100);
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 3)));
	ASSERT(!smoothscroll_clip(0, 3, &top, &end, &left, &right));
	ASSERT(smoothscroll_needs_full());	/* one clean frame at rest */
	ASSERT(!smoothscroll_pending());
	frame_at(1116);
	ASSERT(!smoothscroll_needs_full());
	ASSERT(smoothscroll_timeout() < 0);
}

TEST(region_scroll_down_exposes_departing_bottom_line)
{
	alt_reset(1);
	region_scroll(0, 7, 0, -1);	/* RI at top / IL at top */
	frame_at(1000);
	ASSERT(NEAR(-1.0, smoothscroll_shift(0, 0)));
	smoothscroll_drawextra();
	ASSERT_EQ(1, ndrawn);
	ASSERT_EQ(7, drawn[0]);
	ASSERT_EQ(107, drawnid[0]);
	ASSERT(NEAR(0.0, drawnshift[0]));
}

TEST(region_scrolls_in_one_sync_batch_accumulate)
{
	alt_reset(1);
	fmode = MODE_SYNC;
	fakenow = 1000;
	region_scroll(0, 7, 0, 1);
	fakenow = 1005;
	region_scroll(0, 7, 0, 1);
	fakenow = 1010;
	region_scroll(0, 7, 0, 1);
	frame_at(1020);	/* withheld: nothing advances */
	fmode = 0;
	frame_at(1040);	/* presented: the glide starts now */
	ASSERT(smoothscroll_shift(0, 3) > 2.9);
	ASSERT(smoothscroll_shift(0, 3) <= 3.0);
	smoothscroll_drawextra();
	ASSERT_EQ(3, ndrawn);	/* all three departing lines */
	ASSERT_EQ(102, drawnid[0]);
	ASSERT_EQ(101, drawnid[1]);
	ASSERT_EQ(100, drawnid[2]);
	frame_at(1140);
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 3)));
}

TEST(region_scroll_mid_glide_retargets)
{
	alt_reset(1);
	region_scroll(0, 7, 0, 1);
	frame_at(1000);
	frame_at(1050);
	region_scroll(0, 7, 0, 1);
	frame_at(1050);
	ASSERT(NEAR(1.125, smoothscroll_shift(0, 3)));
	smoothscroll_drawextra();
	ASSERT_EQ(2, ndrawn);	/* both departed lines still sliding out */
	ASSERT_EQ(101, drawnid[0]);
	ASSERT_EQ(100, drawnid[1]);
	/* Reversing direction drops copies that come back as grid rows. */
	region_scroll(0, 7, 0, -1);
	frame_at(1050);
	ASSERT(NEAR(0.125, smoothscroll_shift(0, 3)));
	ndrawn = 0;
	smoothscroll_drawextra();
	ASSERT_EQ(1, ndrawn);
	ASSERT_EQ(100, drawnid[0]);
}

TEST(region_jumps_instead_of_animating)
{
	alt_reset(1);
	region_scroll(0, 7, 0, 8);	/* whole region: a redraw */
	frame_at(1000);
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 3)));
	region_scroll(8, 9, 8, 1);	/* 2-row region */
	frame_at(1000);
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 8)));
	region_scroll(0, 7, 3, 1);	/* DL mid-region: an edit */
	frame_at(1000);
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 5)));
	ASSERT(!smoothscroll_pending());
	/* A fast stream (more than a region per glide) jumps too. */
	for (int i = 0; i < 9; i++)
		region_scroll(0, 7, 0, 1);
	frame_at(1000);
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 3)));
	/* A redraw-sized scroll snaps a glide in progress. */
	region_scroll(0, 7, 0, 2);
	frame_at(1000);
	ASSERT(NEAR(2.0, smoothscroll_shift(0, 3)));
	region_scroll(0, 7, 0, 8);
	frame_at(1000);
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 3)));
}

TEST(region_edit_keeps_glide_and_new_band_restarts)
{
	alt_reset(1);
	region_scroll(0, 7, 0, 2);
	frame_at(1000);
	region_scroll(0, 7, 4, 1);	/* app deletes a line mid-glide */
	frame_at(1010);
	ASSERT(smoothscroll_shift(0, 3) > 1.0);
	region_scroll(0, 5, 0, 1);	/* another window scrolls */
	frame_at(1010);
	ASSERT(NEAR(1.0, smoothscroll_shift(0, 3)));
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 7)));
}

TEST(regions_knob_and_decsclm)
{
	alt_reset(0);
	region_scroll(0, 7, 0, 1);
	frame_at(1000);
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 3)));
	smoothscroll_decsclm(1);	/* app opts in */
	region_scroll(0, 7, 0, 1);
	frame_at(1000);
	ASSERT(NEAR(1.0, smoothscroll_shift(0, 3)));
	frame_at(1200);
	smoothscroll_decsclm(0);
	region_scroll(0, 7, 0, 1);
	frame_at(1200);
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 3)));
	/* With the knob on, CSI ? 4 l does not turn the default off. */
	alt_reset(1);
	smoothscroll_decsclm(0);
	region_scroll(0, 7, 0, 1);
	frame_at(1000);
	ASSERT(NEAR(1.0, smoothscroll_shift(0, 3)));
}

TEST(regions_inert_when_disabled)
{
	int top, end, left, right;

	fake_reset(0);
	falt = 1;
	region_scroll(0, 7, 0, 1);
	frame_at(1000);
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 3)));
	ASSERT(!smoothscroll_clip(0, 3, &top, &end, &left, &right));
	ASSERT(!smoothscroll_band(&top, &end));
	ASSERT(!smoothscroll_plain());
	ASSERT(!smoothscroll_pending());
	smoothscroll_drawextra();
	ASSERT_EQ(0, ndrawn);
	ASSERT_EQ(0, fulldirts);
}

TEST(region_resets_on_screen_switch)
{
	alt_reset(1);
	region_scroll(0, 7, 0, 1);
	frame_at(1000);
	falt = 0;
	frame_at(1010);
	ASSERT(NEAR(0.0, smoothscroll_shift(0, 3)));
	ASSERT(!smoothscroll_pending());
}

/* ---- Static columns: side panels repainted in place ---- */

static Glyph sidebar[FAKE_ROWS][4];

/* Columns 0-3 are a sidebar with distinct rows; 4-11 the scrolled pane. */
static void
sidebar_reset(void)
{
	alt_reset(1);
	for (int y = 0; y < FAKE_ROWS; y++)
		for (int x = 0; x < 4; x++)
			sidebar[y][x] = altstore[y][x] = (Glyph){ .u = 500 + y };
}

static void
sidebar_repaint(void)
{
	for (int y = 0; y < FAKE_ROWS; y++)
		memcpy(altstore[y], sidebar[y], sizeof(sidebar[y]));
}

TEST(sidebar_repainted_in_sync_batch_stays_put)
{
	int top, end, left, right;

	sidebar_reset();
	fmode = MODE_SYNC;	/* the Exocortex TUI: SU, then repaint panel */
	region_scroll(0, 7, 0, 2);
	sidebar_repaint();
	fmode = 0;
	frame_at(1000);
	ASSERT(smoothscroll_span(3, &left, &right));
	ASSERT_EQ(4, left);
	ASSERT_EQ(12, right);
	ASSERT(!smoothscroll_span(9, &left, &right));	/* outside the band */
	ASSERT(NEAR(0.0, smoothscroll_shift(1, 3)));	/* sidebar */
	ASSERT(NEAR(2.0, smoothscroll_shift(6, 3)));	/* pane */
	ASSERT(!smoothscroll_clip(1, 3, &top, &end, &left, &right));
	ASSERT(smoothscroll_clip(6, 3, &top, &end, &left, &right));
	ASSERT_EQ(4, left);
	ASSERT_EQ(12, right);
	/* Departing lines slide out of the pane only, not over the sidebar. */
	smoothscroll_drawextra();
	ASSERT_EQ(2, ndrawn);
	ASSERT_EQ(4, drawnx1[0]);
	ASSERT_EQ(12, drawnx2[0]);
	ASSERT_EQ(101, drawnid[0]);
	ASSERT_EQ(100, drawnid[1]);
}

TEST(rewrapped_right_panel_stays_put)
{
	int left, right;

	alt_reset(1);
	region_scroll(0, 7, 0, 1);
	for (int y = 0; y <= 7; y++)	/* columns 8-11 re-wrapped in place */
		for (int x = 8; x < FAKE_COLS; x++)
			altstore[y][x] = (Glyph){ .u = 900 + 10 * y + x };
	frame_at(1000);
	ASSERT(smoothscroll_span(3, &left, &right));
	ASSERT_EQ(0, left);
	ASSERT_EQ(8, right);
	ASSERT(NEAR(1.0, smoothscroll_shift(2, 3)));
	ASSERT(NEAR(0.0, smoothscroll_shift(9, 3)));
}

TEST(mostly_rewritten_band_does_not_glide)
{
	alt_reset(1);
	region_scroll(0, 7, 0, 1);
	for (int y = 0; y <= 7; y++)
		for (int x = 0; x < FAKE_COLS; x++)
			altstore[y][x] = (Glyph){ .u = 700 + 10 * y + x };
	frame_at(1000);
	ASSERT(NEAR(0.0, smoothscroll_shift(5, 3)));
	ASSERT(!smoothscroll_pending());
	smoothscroll_drawextra();
	ASSERT_EQ(0, ndrawn);
}

TEST(late_panel_repaint_refines_moving_columns)
{
	int left, right;

	sidebar_reset();
	region_scroll(0, 7, 0, 1);	/* presented before the panel repaint */
	frame_at(1000);
	ASSERT(!smoothscroll_span(3, &left, &right));	/* all columns moved */
	sidebar_repaint();
	frame_at(1010);
	ASSERT(smoothscroll_span(3, &left, &right));
	ASSERT_EQ(4, left);
	ASSERT(NEAR(0.0, smoothscroll_shift(1, 3)));
	ASSERT(smoothscroll_shift(6, 3) > 0.5);
}

/* Text repeated on every row of the pane (a prefix, padding) tells nothing
 * apart; it must move with the pane, not split off and stay behind. */
TEST(repeated_pane_text_moves_with_pane)
{
	int left, right;

	sidebar_reset();
	for (int y = 0; y < FAKE_ROWS; y++) {
		/* the sidebar's last column is padding: the gap between panes */
		sidebar[y][3] = altstore[y][3] = (Glyph){ .u = ' ' };
		for (int x = 4; x < 8; x++)
			altstore[y][x] = (Glyph){ .u = 'm' };	/* "msg " on every row */
	}
	fmode = MODE_SYNC;
	region_scroll(0, 7, 0, 1);
	for (int y = 6; y <= 7; y++)	/* the app draws its new row the same way */
		for (int x = 4; x < 8; x++)
			altstore[y][x] = (Glyph){ .u = 'm' };
	sidebar_repaint();
	fmode = 0;
	frame_at(1000);
	ASSERT(smoothscroll_span(3, &left, &right));
	ASSERT_EQ(4, left);	/* split at the padding before the sidebar */
	ASSERT_EQ(12, right);
	ASSERT(NEAR(1.0, smoothscroll_shift(4, 3)));
	ASSERT(NEAR(0.0, smoothscroll_shift(2, 3)));
}

/* A re-wrapped panel's border and label repeat on every row: they tell
 * nothing apart but must stay with the panel, not slide with the pane. */
static void
taskpanel(int y, int k)
{
	altstore[y][8] = (Glyph){ .u = ' ' };	/* padding */
	altstore[y][9] = (Glyph){ .u = 'T' };	/* "│ task" on every row */
	altstore[y][10] = (Glyph){ .u = 900 + 10 * y + k };
	altstore[y][11] = (Glyph){ .u = 950 + 10 * y + k };
}

TEST(rewrapped_panel_keeps_its_repeated_label)
{
	int left, right;

	alt_reset(1);
	for (int y = 0; y < FAKE_ROWS; y++)
		taskpanel(y, 0);
	fmode = MODE_SYNC;
	region_scroll(0, 7, 0, 1);
	for (int y = 0; y <= 7; y++)
		taskpanel(y, 1);
	fmode = 0;
	frame_at(1000);
	ASSERT(smoothscroll_span(3, &left, &right));
	ASSERT_EQ(0, left);
	ASSERT_EQ(8, right);
	ASSERT(NEAR(0.0, smoothscroll_shift(9, 3)));
	ASSERT(NEAR(1.0, smoothscroll_shift(7, 3)));
}

/* A row the app rewrote rather than scrolled (the Exocortex history cursor:
 * a highlight that stays on its screen row) matches its old self in blank
 * columns.  It must not pin those columns of every other row in place: the
 * text of longer lines there would tear off and stay behind. */
static void
cursorline(int y)
{
	for (int x = 4; x < FAKE_COLS; x++)
		altstore[y][x].mode = ATTR_REVERSE;
}

TEST(rewritten_highlight_row_does_not_pin_columns)
{
	int left, right;

	sidebar_reset();
	for (int y = 0; y < FAKE_ROWS; y++)
		for (int x = 9; x < FAKE_COLS; x++)	/* short lines */
			altstore[y][x] = (Glyph){ .u = ' ' };
	cursorline(3);
	fmode = MODE_SYNC;
	region_scroll(0, 7, 0, 1);
	for (int y = 0; y <= 7; y++)
		for (int x = 4; x < FAKE_COLS; x++)
			altstore[y][x].mode = 0;
	for (int x = 9; x < FAKE_COLS; x++)	/* the app's new bottom row */
		altstore[7][x] = (Glyph){ .u = ' ' };
	cursorline(3);
	sidebar_repaint();
	fmode = 0;
	frame_at(1000);
	ASSERT(smoothscroll_span(3, &left, &right));
	ASSERT_EQ(4, left);
	ASSERT_EQ(12, right);
	ASSERT(NEAR(1.0, smoothscroll_shift(10, 5)));
	ASSERT(NEAR(0.0, smoothscroll_shift(1, 5)));
}

/* Every row rewritten (e.g. a highlight on each): nothing scrolled. */
TEST(all_rows_rewritten_does_not_glide)
{
	alt_reset(1);
	region_scroll(0, 7, 0, 1);
	for (int y = 0; y <= 7; y++)
		for (int x = 0; x < FAKE_COLS; x++)
			altstore[y][x].mode = ATTR_REVERSE;
	frame_at(1000);
	ASSERT(NEAR(0.0, smoothscroll_shift(5, 3)));
	ASSERT(!smoothscroll_pending());
}

TEST(uniform_band_glides_full_width)
{
	int left, right;

	alt_reset(1);
	for (int y = 0; y <= 7; y++)
		fillrow(y, 7);	/* e.g. blank lines: nothing to tell apart */
	region_scroll(0, 7, 0, 1);
	frame_at(1000);
	ASSERT(!smoothscroll_span(3, &left, &right));
	ASSERT(NEAR(1.0, smoothscroll_shift(11, 3)));
}

TEST_SUITE(smoothscroll_static_columns)
{
	RUN_TEST(sidebar_repainted_in_sync_batch_stays_put);
	RUN_TEST(rewrapped_right_panel_stays_put);
	RUN_TEST(mostly_rewritten_band_does_not_glide);
	RUN_TEST(late_panel_repaint_refines_moving_columns);
	RUN_TEST(repeated_pane_text_moves_with_pane);
	RUN_TEST(rewrapped_panel_keeps_its_repeated_label);
	RUN_TEST(rewritten_highlight_row_does_not_pin_columns);
	RUN_TEST(all_rows_rewritten_does_not_glide);
	RUN_TEST(uniform_band_glides_full_width);
}

TEST_SUITE(smoothscroll_regions)
{
	RUN_TEST(region_heuristics);
	RUN_TEST(region_scroll_glides_band_only);
	RUN_TEST(region_scroll_down_exposes_departing_bottom_line);
	RUN_TEST(region_scrolls_in_one_sync_batch_accumulate);
	RUN_TEST(region_scroll_mid_glide_retargets);
	RUN_TEST(region_jumps_instead_of_animating);
	RUN_TEST(region_edit_keeps_glide_and_new_band_restarts);
	RUN_TEST(regions_knob_and_decsclm);
	RUN_TEST(regions_inert_when_disabled);
	RUN_TEST(region_resets_on_screen_switch);
}

TEST_SUITE(smoothscroll_engine)
{
	RUN_TEST(ease_runs_from_one_to_exactly_zero);
	RUN_TEST(clamp_never_reveals_past_buffer_ends);
	RUN_TEST(visible_rows_include_partial_rows);
	RUN_TEST(snap_rounds_to_nearest_row);
	RUN_TEST(device_round_snaps_to_physical_pixels);
	RUN_TEST(row_at_follows_the_drawn_grid);
	RUN_TEST(render_path_falls_back_to_cpu);
	RUN_TEST(shift_glides_to_exact_rest);
	RUN_TEST(shift_retargets_from_current_position);
	RUN_TEST(zero_duration_never_animates);
	RUN_TEST(limit_bounds_animation_in_flight);
	RUN_TEST(precise_waits_for_release_then_settles);
	RUN_TEST(precise_takes_over_animation);
}

TEST_SUITE(smoothscroll_terminal)
{
	RUN_TEST(disabled_is_inert);
	RUN_TEST(discrete_scroll_up_glides_and_exposes_rows_below);
	RUN_TEST(discrete_scroll_down_exposes_rows_above);
	RUN_TEST(output_never_animates);
	RUN_TEST(alt_screen_keeps_application_scrolling);
	RUN_TEST(big_jump_glides_from_one_screen);
	RUN_TEST(pixels_follow_input_and_settle_on_a_row);
	RUN_TEST(pixels_stop_at_buffer_ends);
	RUN_TEST(resize_resets_offset);
}

int
main(void)
{
	RUN_SUITE(smoothscroll_engine);
	RUN_SUITE(smoothscroll_terminal);
	RUN_SUITE(smoothscroll_regions);
	RUN_SUITE(smoothscroll_static_columns);
	return test_summary();
}
