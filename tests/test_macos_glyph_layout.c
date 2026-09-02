#include <math.h>

#include "test.h"
#include "../macos/glyph_layout.h"

#define ASSERT_NEAR(expected, actual) \
	ASSERT(fabs((expected) - (actual)) < 0.000001)

TEST(color_emoji_upscales_to_fill_wide_cell)
{
	/*
	 * Representative Retina metrics for SF Mono 11 and Apple Color Emoji:
	 * a 22 px ink box plus two pixels of atlas padding on each side, in a
	 * two-column 28 x 28 px terminal cell.
	 */
	MacGlyphRect rect = macos_color_glyph_rect(10, 20, 28, 28,
	    26, 26, 22, 22);

	ASSERT(rect.width > 26);
	ASSERT(rect.height > 26);
	ASSERT_NEAR(28 * MACOS_COLOR_GLYPH_CELL_FILL,
	    rect.width * 22 / 26);
}

TEST(color_emoji_is_centered_on_cell_not_baseline)
{
	MacGlyphRect rect = macos_color_glyph_rect(10, 40, 28, 28,
	    26, 26, 22, 22);
	double ink_x = rect.x + (rect.width - rect.width * 22 / 26) / 2.0;
	double ink_y = rect.y + (rect.height - rect.height * 22 / 26) / 2.0;
	double ink_width = rect.width * 22 / 26;
	double ink_height = rect.height * 22 / 26;

	ASSERT_NEAR(10 + (28 - rect.width) / 2.0, rect.x);
	ASSERT_NEAR(40 + (28 - rect.height) / 2.0, rect.y);
	ASSERT(ink_x >= 10);
	ASSERT(ink_y >= 40);
	ASSERT(ink_x + ink_width <= 38);
	ASSERT(ink_y + ink_height <= 68);
}

TEST(color_emoji_ink_stays_inside_narrow_cell)
{
	MacGlyphRect rect = macos_color_glyph_rect(0, 0, 14, 28,
	    26, 26, 22, 22);
	double ink_width = rect.width * 22 / 26;
	double ink_height = rect.height * 22 / 26;

	ASSERT(ink_width <= 14);
	ASSERT(ink_height <= 28);
	ASSERT_NEAR(14 * MACOS_COLOR_GLYPH_CELL_FILL, ink_width);
}

TEST(filled_block_covers_the_exact_cell)
{
	MacGlyphRect rect;

	ASSERT(macos_filled_block_rect(0x2588, 10, 20, 7, 15, 2, &rect));
	ASSERT_NEAR(10, rect.x);
	ASSERT_NEAR(20, rect.y);
	ASSERT_NEAR(7, rect.width);
	ASSERT_NEAR(15, rect.height);
}

TEST(half_blocks_meet_at_an_even_backing_pixel_boundary)
{
	MacGlyphRect upper, lower;

	ASSERT(macos_filled_block_rect(0x2580, 10, 20, 7, 15, 2, &upper));
	ASSERT(macos_filled_block_rect(0x2584, 10, 20, 7, 15, 2, &lower));
	ASSERT_NEAR(7.5, upper.height);
	ASSERT_NEAR(27.5, upper.y + upper.height);
	ASSERT_NEAR(27.5, lower.y);
	ASSERT_NEAR(7.5, lower.height);
}

TEST(half_blocks_overlap_an_odd_device_pixel_to_prevent_seams)
{
	MacGlyphRect upper, lower;

	ASSERT(macos_filled_block_rect(0x2580, 10, 20, 7, 15, 1, &upper));
	ASSERT(macos_filled_block_rect(0x2584, 10, 20, 7, 15, 1, &lower));
	ASSERT_NEAR(8, upper.height);
	ASSERT_NEAR(28, upper.y + upper.height);
	ASSERT_NEAR(27, lower.y);
	ASSERT_NEAR(8, lower.height);
}

TEST(ordinary_glyph_is_not_treated_as_a_filled_block)
{
	MacGlyphRect rect;

	ASSERT(!macos_filled_block_rect('A', 10, 20, 7, 15, 2, &rect));
}

TEST_SUITE(macos_glyph_layout)
{
	RUN_TEST(color_emoji_upscales_to_fill_wide_cell);
	RUN_TEST(color_emoji_is_centered_on_cell_not_baseline);
	RUN_TEST(color_emoji_ink_stays_inside_narrow_cell);
	RUN_TEST(filled_block_covers_the_exact_cell);
	RUN_TEST(half_blocks_meet_at_an_even_backing_pixel_boundary);
	RUN_TEST(half_blocks_overlap_an_odd_device_pixel_to_prevent_seams);
	RUN_TEST(ordinary_glyph_is_not_treated_as_a_filled_block);
}

int
main(void)
{
	RUN_SUITE(macos_glyph_layout);
	return test_summary();
}
