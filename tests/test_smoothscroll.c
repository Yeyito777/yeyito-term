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
#define FAKE_COLS 4
#define FAKE_LINES (FAKE_HIST + FAKE_ROWS)

static Glyph store[FAKE_LINES][FAKE_COLS], altstore[FAKE_ROWS][FAKE_COLS];
static Line lines[FAKE_LINES];
static int fhistn, fscr, frows, falt, fulldirts, ndrawn, drawn[64];
static double fakenow;

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
	(void)line; (void)x1; (void)x2;
	if (ndrawn < (int)(sizeof(drawn) / sizeof(drawn[0])))
		drawn[ndrawn++] = y;
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
	smoothscroll_configure(enabled, 100);
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
	ASSERT(smoothscroll_offset_px(20, 1) == 0.0);
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
	ASSERT(NEAR(-60.0, smoothscroll_offset_px(20, 1)));
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
	ASSERT(NEAR(ss_device_round(-60.0 * 0.125, 1), smoothscroll_offset_px(20, 1)));
	frame_at(1100);
	ASSERT(smoothscroll_offset_px(20, 1) == 0.0);
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
	ASSERT(NEAR(40.0, smoothscroll_offset_px(20, 1)));
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
	ASSERT(smoothscroll_offset_px(20, 1) == 0.0);
	kscrollup(&(Arg){ .i = 4 });
	frame_at(1000);
	frame_at(1200);
	output_line();	/* scrolled up: view keeps its lines, scr grows */
	output_line();
	frame_at(1300);
	ASSERT_EQ(6, fscr);
	ASSERT(smoothscroll_offset_px(20, 1) == 0.0);
	ASSERT(!smoothscroll_pending());
}

TEST(alt_screen_keeps_application_scrolling)
{
	fake_reset(1);
	falt = 1;
	frame_at(1000);
	kscrollup(&(Arg){ .i = 3 });
	frame_at(1000);
	ASSERT(smoothscroll_offset_px(20, 1) == 0.0);
	ASSERT_EQ(0, smoothscroll_pixels(30, 20, 1));
	ASSERT(!smoothscroll_pending());
	/* Leaving the alternate screen never animates the switch. */
	falt = 0;
	frame_at(1010);
	ASSERT(smoothscroll_offset_px(20, 1) == 0.0);
}

TEST(big_jump_glides_from_one_screen)
{
	fake_reset(1);
	kscrollup(&(Arg){ .i = 80 });
	frame_at(1000);
	ASSERT(NEAR(-200.0, smoothscroll_offset_px(20, 1)));
	frame_at(1100);
	kscrolldown(&(Arg){ .i = 80 });
	frame_at(1200);
	ASSERT(NEAR(200.0, smoothscroll_offset_px(20, 1)));
}

TEST(pixels_follow_input_and_settle_on_a_row)
{
	fake_reset(1);
	ASSERT_EQ(1, smoothscroll_pixels(30, 20, 1));	/* 1.5 rows up */
	ASSERT_EQ(2, fscr);
	ASSERT(NEAR(-10.0, smoothscroll_offset_px(20, 1)));
	frame_at(5000);	/* fingers still down: never snaps */
	ASSERT(NEAR(-10.0, smoothscroll_offset_px(20, 1)));
	ASSERT(!smoothscroll_pending());
	smoothscroll_release();
	ASSERT(smoothscroll_pending());
	frame_at(5030);	/* momentum window still open */
	ASSERT(NEAR(-10.0, smoothscroll_offset_px(20, 1)));
	frame_at(5061);	/* settles: snap to the nearest row, glide there */
	ASSERT(smoothscroll_pending());
	frame_at(5200);
	ASSERT(smoothscroll_offset_px(20, 1) == 0.0);
	ASSERT(fscr == 1 || fscr == 2);
	ASSERT(!smoothscroll_pending());
}

TEST(pixels_stop_at_buffer_ends)
{
	fake_reset(1);
	ASSERT_EQ(1, smoothscroll_pixels(-30, 20, 1));	/* below the bottom */
	ASSERT_EQ(0, fscr);
	ASSERT(smoothscroll_offset_px(20, 1) == 0.0);
	ASSERT_EQ(1, smoothscroll_pixels(20 * 500, 20, 1));	/* past the top */
	ASSERT_EQ(FAKE_HIST, fscr);
	ASSERT(smoothscroll_offset_px(20, 1) == 0.0);
}

TEST(resize_resets_offset)
{
	fake_reset(1);
	kscrollup(&(Arg){ .i = 3 });
	frame_at(1000);
	ASSERT(smoothscroll_offset_px(20, 1) != 0.0);
	frows = 8;
	frame_at(1010);
	ASSERT(smoothscroll_offset_px(20, 1) == 0.0);
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
	return test_summary();
}
