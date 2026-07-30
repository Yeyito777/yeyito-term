#ifndef MACOS_GLYPH_LAYOUT_H
#define MACOS_GLYPH_LAYOUT_H

#define MACOS_COLOR_GLYPH_CELL_FILL 0.94

typedef struct {
	double x;
	double y;
	double width;
	double height;
} MacGlyphRect;

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
