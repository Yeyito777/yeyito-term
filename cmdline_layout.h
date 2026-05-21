/* See LICENSE for license details. */
/* Pure geometry helpers for the command-line overlay. */

#ifndef CMDLINE_LAYOUT_H
#define CMDLINE_LAYOUT_H

typedef struct {
	int y;
	int width;
	int height;
	int row_height;
	int baseline;
} CmdlineLayout;

static inline int
cmdline_clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static inline CmdlineLayout
cmdline_layout(int win_w, int win_h, int row_top, int row_bottom,
               int font_ascent, int font_descent, int border_top)
{
	CmdlineLayout l;
	int content_top, content_height, line_height;
	int min_baseline, max_baseline;

	if (row_top < 0)
		row_top = 0;
	if (row_top > win_h)
		row_top = win_h;
	if (row_bottom < row_top)
		row_bottom = row_top;
	if (row_bottom > win_h)
		row_bottom = win_h;
	if (font_ascent < 0)
		font_ascent = 0;
	if (font_descent < 0)
		font_descent = 0;
	if (border_top < 0)
		border_top = 0;

	l.y = row_top;
	l.width = win_w;
	l.height = win_h - row_top;
	l.row_height = row_bottom - row_top;
	if (l.height < 1)
		l.height = 1;
	if (l.row_height < 1)
		l.row_height = 1;

	content_top = cmdline_clamp(border_top, 0, l.row_height - 1);
	content_height = l.row_height - content_top;
	if (content_height < 1)
		content_height = 1;
	line_height = font_ascent + font_descent;
	if (line_height < 1)
		line_height = 1;

	/* The command line is drawn with Xft even when the terminal grid is drawn by
	 * the GPU.  Center the Xft font inside the *actual rendered bottom row* so
	 * it does not inherit stale integer-grid geometry or GPU-specific baselines. */
	l.baseline = content_top + (content_height - line_height) / 2 + font_ascent;
	min_baseline = font_ascent;
	max_baseline = l.height - font_descent;
	if (max_baseline < 0)
		max_baseline = 0;
	if (min_baseline > max_baseline)
		l.baseline = max_baseline;
	else
		l.baseline = cmdline_clamp(l.baseline, min_baseline, max_baseline);
	return l;
}

#endif /* CMDLINE_LAYOUT_H */
