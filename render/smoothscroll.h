/* See LICENSE for license details. */
/* Pixel-smooth scrolling of the scrollback view and of application scroll
 * regions (see smoothscroll_term.c).
 *
 * The terminal's logical scroll position (term.scr, whole rows) always changes
 * immediately, exactly as without this feature.  Smooth scrolling is purely a
 * presentation layer: a fractional row offset by which the renderer translates
 * the grid, plus the partial rows that translation exposes.  It decays to zero
 * after a discrete scroll and follows the fingers for precise (trackpad) input,
 * so every row-based feature (selection, search, vimnav, images, cursor) keeps
 * using the same row coordinates and moves with the grid.
 *
 * Offsets are in rows.  Positive offsets draw the grid lower, exposing older
 * history above row 0; negative offsets draw it higher, exposing the rows
 * below the last one.  See reference/smooth-scroll.md. */

#ifndef SMOOTHSCROLL_H
#define SMOOTHSCROLL_H

/* ---- Pure engine: no terminal or platform dependencies (unit tested). ---- */

enum {
	SS_IDLE,      /* offset is 0 and nothing is scheduled */
	SS_ANIMATING, /* offset decays from `from` to 0 */
	SS_PRECISE,   /* precise input drives the offset directly */
};

enum {
	SS_RENDER_OFF, /* feature disabled: renderers are untouched */
	SS_RENDER_GPU, /* GL/Metal shift and clip row vertices */
	SS_RENDER_CPU, /* Xft draws rows offset under a clip into the pixmap */
};

typedef struct {
	int state;
	double offset;    /* rows */
	double from;      /* offset when the current animation started */
	double start;     /* ms */
	double duration;  /* ms */
	double settle_at; /* ms: precise input snaps to a row once idle */
	int held;         /* fingers are on the trackpad: never snap */
} SmoothScroll;

double ss_ease(double progress);
double ss_clamp(double offset, int above, int below);
void ss_visible_rows(double offset, int rows, int *first, int *last);
int ss_snap_rows(double offset);
double ss_device_round(double pixels, double scale);
int ss_row_at(double y, double top, double cellh, double offsetpx);
int ss_region_animates(int alt, int rows, int top, int bot, int orig, int n);
int ss_render_path(int enabled, int gpu_active);

void ss_reset(SmoothScroll *ss);
void ss_shift(SmoothScroll *ss, double rows, double now, double duration);
void ss_limit(SmoothScroll *ss, int above, int below);
void ss_precise(SmoothScroll *ss, double rows, double now, int held,
		double settle);
int ss_settle_due(const SmoothScroll *ss, double now);
void ss_settle(SmoothScroll *ss, double now, double duration);
int ss_tick(SmoothScroll *ss, double now);
double ss_wait(const SmoothScroll *ss, double now);

/* ---- Terminal glue (render/smoothscroll_term.c). ---- */

/* regions: animate application region scrolls (smoothscroll_regions). */
void smoothscroll_configure(int enabled, double duration_ms, int regions);
int smoothscroll_enabled(void);
/* DECSCLM (CSI ? 4 h/l): application opt-in to region animation. */
void smoothscroll_decsclm(int set);
/* tscrollup()/tscrolldown(), before rows [orig, bot] of the scroll region
 * [top, bot] move by n rows (positive: content moves up). */
void smoothscroll_region(int top, int bot, int orig, int n);
/* Start of draw(): follow logical scroll changes, advance the animations and
 * mark the grid dirty while anything moves.  Cheap when nothing scrolled. */
void smoothscroll_frame(void);
/* Grid rows [first, end) visible this frame; [0, rows) at rest. */
void smoothscroll_rows(int *first, int *end);
/* After drawregion(): draw the partial rows / departing lines exposed. */
void smoothscroll_drawextra(void);
/* Renderers, per grid row y being drawn: shift it by this many rows (0 at
 * rest) and, when row_clip() returns 1, clip it to grid rows [top, end)
 * (unshifted).  While plain() is 1 the line drawn is a departing copy, not a
 * grid row: skip selection/search/line highlights for it. */
double smoothscroll_row_shift(int y);
int smoothscroll_row_clip(int y, int *top, int *end);
int smoothscroll_plain(void);
/* Rows [top, end) that move this frame (CPU renderers clear them first). */
int smoothscroll_band(int *top, int *end);
/* Scrollback shift in rows, for mapping the mouse to the drawn grid. */
double smoothscroll_view_shift(void);
/* Something moved this frame or the last: redraw the grid completely
 * instead of trusting retained damage. */
int smoothscroll_needs_full(void);
/* Frames are still needed (animation or pending settle). */
int smoothscroll_pending(void);
/* Milliseconds until the next frame is due (0 while animating: the caller
 * paces frames, e.g. at the display refresh), or -1 when idle. */
double smoothscroll_timeout(void);
/* Precise (pixel) input of `pixels` for rows of `cellh` pixels; positive
 * scrolls towards older history.  Returns 0 when the caller should fall back
 * to discrete scrolling (feature off, alternate screen). */
int smoothscroll_pixels(double pixels, double cellh, int held);
/* Fingers lifted / gesture cancelled: snap once momentum stops. */
void smoothscroll_release(void);

#endif /* SMOOTHSCROLL_H */
