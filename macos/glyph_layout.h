#ifndef MACOS_GLYPH_LAYOUT_H
#define MACOS_GLYPH_LAYOUT_H

#include <math.h>
#include <stdint.h>

#define MACOS_COLOR_GLYPH_CELL_FILL 0.94

typedef struct {
	double x;
	double y;
	double width;
	double height;
} MacGlyphRect;

/*
 * Filled block elements are terminal-cell graphics rather than ordinary text.
 * Snap them to backing pixels and construct them from rectangles so adjacent
 * QR-code cells have neither font side bearings nor antialiased seams. For an
 * odd device-pixel height, the half blocks overlap by one pixel, matching the
 * X11 renderer's integer geometry.
 */
static inline int
macos_filled_block_rect(uint32_t rune, double cell_x, double cell_y,
		double cell_width, double cell_height, double backing_scale,
		MacGlyphRect *rect)
{
	double left, top, right, bottom, height;

	if (!rect || (rune != 0x2580 && rune != 0x2584 && rune != 0x2588))
		return 0;
	if (backing_scale <= 0)
		backing_scale = 1;

	left = round(cell_x * backing_scale);
	top = round(cell_y * backing_scale);
	right = round((cell_x + cell_width) * backing_scale);
	bottom = round((cell_y + cell_height) * backing_scale);
	height = bottom - top;

	if (rune == 0x2580) {
		bottom = top + floor((height + 1) / 2);
	} else if (rune == 0x2584) {
		top += floor(height / 2);
	}

	*rect = (MacGlyphRect){
		.x = left / backing_scale,
		.y = top / backing_scale,
		.width = (right - left) / backing_scale,
		.height = (bottom - top) / backing_scale,
	};
	return 1;
}

/*
 * Color-font glyph atlases include transparent padding around the actual ink.
 * Fit the ink to the terminal cell, then center the padded texture on the
 * cell.  The transparent texture edge may extend beyond the cell, but the
 * visible glyph remains inside it.
 */
static inline MacGlyphRect
macos_color_glyph_rect(double cell_x, double cell_y,
		double cell_width, double cell_height,
		double texture_width, double texture_height,
		double ink_width, double ink_height)
{
	MacGlyphRect rect = {
		.x = cell_x,
		.y = cell_y,
		.width = texture_width,
		.height = texture_height,
	};

	if (cell_width <= 0 || cell_height <= 0 ||
	    texture_width <= 0 || texture_height <= 0 ||
	    ink_width <= 0 || ink_height <= 0)
		return rect;

	double width_scale = cell_width / ink_width;
	double height_scale = cell_height / ink_height;
	double scale = (width_scale < height_scale ?
	    width_scale : height_scale) * MACOS_COLOR_GLYPH_CELL_FILL;

	rect.width *= scale;
	rect.height *= scale;
	rect.x = cell_x + (cell_width - rect.width) / 2.0;
	rect.y = cell_y + (cell_height - rect.height) / 2.0;
	return rect;
}

#endif
