/* See LICENSE for license details. */
/* Test for scrollback history preservation on screen clear */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "mocks.h"

/* TLINE macro for accessing history/screen lines */
#define TLINE(y)		((y) < term.scr ? term.hist[((y) + term.histi - \
				term.scr + HISTSIZE + 1) % HISTSIZE] : \
				term.line[(y) - term.scr])

/* IS_SET macro */
#define IS_SET(flag)		((term.mode & (flag)) != 0)

/* Helper to set a line with content */
static void
set_line_content(int y, const char *text)
{
	int i, len;

	if (y < 0 || y >= term.row || !term.line[y])
		return;

	len = strlen(text);
	if (len > term.col)
		len = term.col;

	for (i = 0; i < len; i++) {
		term.line[y][i].u = text[i];
	}
}

/* Helper to get first char from a line */
static Rune
get_line_first_char(int y)
{
	if (y < 0 || y >= term.row || !term.line[y])
		return 0;
	return term.line[y][0].u;
}

/* Helper to get first char from history line */
static Rune
get_hist_first_char(int idx)
{
	idx = (idx + HISTSIZE) % HISTSIZE;
	if (!term.hist[idx])
		return 0;
	return term.hist[idx][0].u;
}

/*
 * Simulate ED case 2 (clear all) with the FIX:
 * Scroll content lines into history, excluding last line (prompt)
 */
static void
ed_clear_all_fixed(void)
{
	if (!IS_SET(MODE_ALTSCREEN)) {
		int i, last_content = -1;
		/* Find last line with content */
		for (i = term.row - 1; i >= 0; i--) {
			if (tlinelen(i) > 0) {
				last_content = i;
				break;
			}
		}
		/* Scroll content lines into history, excluding the last
		 * content line (assumed to be prompt, will be redrawn) */
		for (i = 0; i < last_content; i++)
			tscrollup(0, 1, 1);
		/* Clear the screen */
		tclearregion(0, 0, term.col - 1, term.row - 1);
	} else {
		tclearregion(0, 0, term.col - 1, term.row - 1);
	}
}

/*
 * Simulate ED case 2 (clear all) with OLD behavior:
 * Just clear the region without saving to history
 */
static void
ed_clear_all_old(void)
{
	tclearregion(0, 0, term.col - 1, term.row - 1);
}

/* Test: Old behavior loses screen content */
TEST(old_clear_loses_content)
{
	mock_term_init(5, 10);

	/* Set up screen with content */
	set_line_content(0, "Line 0");
	set_line_content(1, "Line 1");
	set_line_content(2, "Line 2");
	set_line_content(3, "Line 3");
	set_line_content(4, "Line 4");

	/* Verify content is there */
	ASSERT_EQ('L', get_line_first_char(0));
	ASSERT_EQ('L', get_line_first_char(4));

	/* Clear with old behavior */
	ed_clear_all_old();

	/* Screen should be cleared */
	ASSERT_EQ(' ', get_line_first_char(0));
	ASSERT_EQ(' ', get_line_first_char(4));

	/* History should be empty (histi still 0, no content saved) */
	ASSERT_EQ(0, term.histi);

	mock_term_free();
}

/* Test: Fixed behavior preserves screen content in history, skips prompt */
TEST(fixed_clear_preserves_content)
{
	mock_term_init(5, 10);

	/* Set up screen with content: 4 lines of output + 1 prompt line */
	set_line_content(0, "Line 0");
	set_line_content(1, "Line 1");
	set_line_content(2, "Line 2");
	set_line_content(3, "Line 3");
	set_line_content(4, "% prompt");  /* This is the prompt, should be skipped */

	/* Verify content is there */
	ASSERT_EQ('L', get_line_first_char(0));
	ASSERT_EQ('%', get_line_first_char(4));

	/* Clear with fixed behavior */
	ed_clear_all_fixed();

	/* Screen should be cleared */

	/* History should have lines 0-3 (4 lines), prompt skipped */
	ASSERT_EQ(4, term.histi);

	/* Check that output was saved to history (prompt was NOT saved) */
	/* Most recent (histi=4) has what was Line 3 */
	ASSERT_EQ('L', get_hist_first_char(4));  /* Line 3 */
	ASSERT_EQ('L', get_hist_first_char(3));  /* Line 2 */
	ASSERT_EQ('L', get_hist_first_char(2));  /* Line 1 */
	ASSERT_EQ('L', get_hist_first_char(1));  /* Line 0 */

	mock_term_free();
}

/* Test: Alt screen clear doesn't save to history */
TEST(altscreen_clear_no_history)
{
	mock_term_init(5, 10);

	/* Enable alt screen mode */
	term.mode |= MODE_ALTSCREEN;

	/* Set up screen with content */
	set_line_content(0, "Alt 0");
	set_line_content(1, "Alt 1");

	/* Clear with fixed behavior - should NOT save to history */
	ed_clear_all_fixed();

	/* Screen should be cleared */
	ASSERT_EQ(' ', get_line_first_char(0));
	ASSERT_EQ(' ', get_line_first_char(1));

	/* History should NOT be touched (histi still 0) */
	ASSERT_EQ(0, term.histi);

	mock_term_free();
}

/* Test: Multiple clears accumulate in history */
TEST(multiple_clears_accumulate)
{
	mock_term_init(3, 10);

	/* First set: 2 output lines + 1 prompt */
	set_line_content(0, "AAA");
	set_line_content(1, "BBB");
	set_line_content(2, "% prompt1");  /* skipped */

	/* First clear - saves lines 0-1, skips line 2 */
	ed_clear_all_fixed();
	ASSERT_EQ(2, term.histi);

	/* Second set: 2 output lines + 1 prompt */
	set_line_content(0, "DDD");
	set_line_content(1, "EEE");
	set_line_content(2, "% prompt2");  /* skipped */

	/* Second clear - saves lines 0-1, skips line 2 */
	ed_clear_all_fixed();
	ASSERT_EQ(4, term.histi);

	/* History should have both output sets (not prompts) */
	/* Most recent: E, D (indices 4, 3) */
	/* Older: B, A (indices 2, 1) */
	ASSERT_EQ('E', get_hist_first_char(4));
	ASSERT_EQ('D', get_hist_first_char(3));
	ASSERT_EQ('B', get_hist_first_char(2));
	ASSERT_EQ('A', get_hist_first_char(1));

	mock_term_free();
}

/* Test: Small output saves content, skips prompt, ignores empty lines */
TEST(small_output_only_saves_content)
{
	mock_term_init(10, 20);  /* 10 row terminal */

	/* Small output like "seq 2": 2 lines of output + prompt */
	set_line_content(0, "1");
	set_line_content(1, "2");
	set_line_content(2, "% ");  /* prompt - should be skipped */
	/* Lines 3-9 are empty */

	/* Clear with fixed behavior */
	ed_clear_all_fixed();

	/* Only output lines should be saved (not empty lines, not prompt) */
	ASSERT_EQ(2, term.histi);

	/* Check output content was saved (prompt was NOT saved) */
	ASSERT_EQ('2', get_hist_first_char(2));  /* Line 1 content */
	ASSERT_EQ('1', get_hist_first_char(1));  /* Line 0 content */

	mock_term_free();
}

/* Test: Empty screen doesn't save anything */
TEST(empty_screen_no_history)
{
	mock_term_init(5, 10);

	/* All lines empty */
	/* Clear should not save anything */
	ed_clear_all_fixed();

	ASSERT_EQ(0, term.histi);

	mock_term_free();
}

/* Test: Spamming Ctrl+L doesn't fill history with prompts */
TEST(spam_clear_no_prompt_accumulation)
{
	mock_term_init(5, 10);

	/* Just a prompt on screen (fresh terminal or after previous clear) */
	set_line_content(0, "% ");

	/* Clear multiple times - should NOT save the prompt each time */
	ed_clear_all_fixed();
	ASSERT_EQ(0, term.histi);  /* Nothing saved - only prompt, no output */

	/* Simulate prompt being redrawn */
	set_line_content(0, "% ");

	ed_clear_all_fixed();
	ASSERT_EQ(0, term.histi);  /* Still nothing - just prompts */

	ed_clear_all_fixed();
	ASSERT_EQ(0, term.histi);  /* Still nothing */

	mock_term_free();
}

/* Test suite */
TEST_SUITE(scrollback)
{
	RUN_TEST(old_clear_loses_content);
	RUN_TEST(fixed_clear_preserves_content);
	RUN_TEST(altscreen_clear_no_history);
	RUN_TEST(multiple_clears_accumulate);
	RUN_TEST(small_output_only_saves_content);
	RUN_TEST(empty_screen_no_history);
	RUN_TEST(spam_clear_no_prompt_accumulation);
}

int
main(void)
{
	printf("st scrollback test suite\n");
	printf("========================================\n");

	RUN_SUITE(scrollback);

	return test_summary();
}
