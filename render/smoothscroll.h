/* See LICENSE for license details. */
/* Pixel-smooth scrolling of the scrollback view.
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
	SS_RENDER_GPU, /* GL/Metal translate the grid and scissor it */
	SS_RENDER_CPU, /* Xft draws rows at an offset into a clipped pixmap */
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

void smoothscroll_configure(int enabled, double duration_ms);
int smoothscroll_enabled(void);
/* Start of draw(): follow logical scroll changes, advance the animation and
 * mark the grid dirty while it moves.  Cheap no-op when nothing scrolled. */
void smoothscroll_frame(void);
/* Grid rows [first, end) visible this frame; [0, rows) at rest. */
void smoothscroll_rows(int *first, int *end);
/* After drawregion(): draw the partial rows the offset exposes. */
void smoothscroll_drawextra(void);
/* Current offset in device pixels for a row of `cellh` pixels; 0 at rest. */
double smoothscroll_offset_px(double cellh, double scale);
/* The grid is translated this frame (or was last frame): clip and redraw it
 * completely instead of trusting retained damage. */
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
