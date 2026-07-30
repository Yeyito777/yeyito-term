#include <string.h>

#include "test.h"
#include "../macos/emoji.h"

static Glyph line[8];

static void
reset_line(void)
{
	memset(line, 0, sizeof(line));
	for (int i = 0; i < (int)LEN(line); i++)
		line[i].u = ' ';
}

TEST(emoji_selector_marks_previous_glyph)
{
	reset_line();
	line[2].u = 0x2699;

	ASSERT_EQ(2, macos_apply_presentation_selector(line, LEN(line), 3, 0,
	    MACOS_EMOJI_PRESENTATION_SELECTOR));
	ASSERT(line[2].mode & ATTR_EMOJI);
}

TEST(emoji_selector_finds_wide_glyph_before_dummy)
{
	reset_line();
	line[2].u = 0x1f600;
	line[2].mode = ATTR_WIDE;
	line[3].mode = ATTR_WDUMMY;

	ASSERT_EQ(2, macos_apply_presentation_selector(line, LEN(line), 4, 0,
	    MACOS_EMOJI_PRESENTATION_SELECTOR));
	ASSERT(line[2].mode & ATTR_EMOJI);
}

TEST(emoji_selector_handles_wrap_pending_cursor)
{
	reset_line();
	line[7].u = 0x2699;

	ASSERT_EQ(7, macos_apply_presentation_selector(line, LEN(line), 7, 1,
	    MACOS_EMOJI_PRESENTATION_SELECTOR));
	ASSERT(line[7].mode & ATTR_EMOJI);
}

TEST(text_selector_clears_emoji_presentation)
{
	reset_line();
	line[2].u = 0x2699;
	line[2].mode = ATTR_EMOJI;

	ASSERT_EQ(2, macos_apply_presentation_selector(line, LEN(line), 3, 0,
	    MACOS_TEXT_PRESENTATION_SELECTOR));
	ASSERT(!(line[2].mode & ATTR_EMOJI));
}

TEST(selector_without_base_is_ignored)
{
	reset_line();

	ASSERT_EQ(-1, macos_apply_presentation_selector(line, LEN(line), 0, 0,
	    MACOS_EMOJI_PRESENTATION_SELECTOR));
	for (int i = 0; i < (int)LEN(line); i++)
		ASSERT(!(line[i].mode & ATTR_EMOJI));
}

TEST(emoji_presentation_expands_text_width_glyph)
{
	reset_line();
	line[2].u = 0x2699;

	ASSERT(macos_expand_emoji_width(line, LEN(line), 2));
	ASSERT(line[2].mode & ATTR_WIDE);
	ASSERT_EQ(ATTR_WDUMMY, line[3].mode);
	ASSERT_EQ(0, line[3].u);
}

TEST(emoji_width_does_not_expand_past_line_end)
{
	reset_line();
	line[7].u = 0x2699;

	ASSERT(!macos_expand_emoji_width(line, LEN(line), 7));
	ASSERT(!(line[7].mode & ATTR_WIDE));
}

TEST_SUITE(macos_emoji)
{
	RUN_TEST(emoji_selector_marks_previous_glyph);
	RUN_TEST(emoji_selector_finds_wide_glyph_before_dummy);
	RUN_TEST(emoji_selector_handles_wrap_pending_cursor);
	RUN_TEST(text_selector_clears_emoji_presentation);
	RUN_TEST(selector_without_base_is_ignored);
	RUN_TEST(emoji_presentation_expands_text_width_glyph);
	RUN_TEST(emoji_width_does_not_expand_past_line_end);
}

int
main(void)
{
	RUN_SUITE(macos_emoji);
	return test_summary();
}
