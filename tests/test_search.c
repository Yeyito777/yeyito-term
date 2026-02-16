/* See LICENSE for license details. */
/* Tests for search.c — regex search through scrollback */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "test.h"
#include "../st.h"
#include "../vimnav.h"

/* --- Globals and mocks needed by search.c --- */

VimNav vimnav;
int debug_mode = 0;
unsigned int search_match_bg = 264;

/* Mock functions needed by search.c */
void tfulldirt(void) { }

size_t
utf8encode(Rune u, char *c)
{
	if (u < 0x80) {
		c[0] = u;
		return 1;
	} else if (u < 0x800) {
		c[0] = 0xC0 | (u >> 6);
		c[1] = 0x80 | (u & 0x3F);
		return 2;
	} else if (u < 0x10000) {
		c[0] = 0xE0 | (u >> 12);
		c[1] = 0x80 | ((u >> 6) & 0x3F);
		c[2] = 0x80 | (u & 0x3F);
		return 3;
	} else {
		c[0] = 0xF0 | (u >> 18);
		c[1] = 0x80 | ((u >> 12) & 0x3F);
		c[2] = 0x80 | ((u >> 6) & 0x3F);
		c[3] = 0x80 | (u & 0x3F);
		return 4;
	}
}

/*
 * Include search.c directly (self-contained test pattern).
 * search.c defines Term_search and declares 'extern Term_search term'.
 */
#include "../search.c"

/* Provide the actual term variable (satisfies the extern in search.c) */
Term_search term;

/* --- Test helpers --- */

#define TEST_COLS 80
#define TEST_ROWS 24

static Glyph test_screen[TEST_ROWS][TEST_COLS];
static Line test_lines[TEST_ROWS];
static int test_dirty[TEST_ROWS];

static void
test_init(void)
{
	int y;

	memset(&term, 0, sizeof(term));
	memset(&vimnav, 0, sizeof(vimnav));
	memset(test_screen, 0, sizeof(test_screen));

	term.row = TEST_ROWS;
	term.col = TEST_COLS;
	term.dirty = test_dirty;

	for (y = 0; y < TEST_ROWS; y++) {
		test_lines[y] = test_screen[y];
		term.dirty[y] = 0;
	}
	term.line = test_lines;
	term.scr = 0;
	term.histn = 0;
	term.histi = 0;

	vimnav.mode = 1; /* VIMNAV_NORMAL */
	vimnav.x = 0;
	vimnav.y = 0;

	search_clear();
}

static void
set_line_text(int y, const char *text)
{
	int col = 0;
	const unsigned char *p = (const unsigned char *)text;

	while (*p && col < TEST_COLS) {
		test_screen[y][col].u = *p;
		test_screen[y][col].mode = 0;
		test_screen[y][col].fg = 0;
		test_screen[y][col].bg = 0;
		p++;
		col++;
	}
	/* Fill rest with spaces */
	while (col < TEST_COLS) {
		test_screen[y][col].u = ' ';
		test_screen[y][col].mode = 0;
		test_screen[y][col].fg = 0;
		test_screen[y][col].bg = 0;
		col++;
	}
}

/* --- Tests --- */

TEST(search_inactive_by_default) {
	test_init();
	ASSERT_EQ(0, search_active());
	ASSERT_EQ(0, search_matched(0, 0));
}

TEST(search_basic_forward) {
	test_init();
	set_line_text(0, "hello world");
	set_line_text(1, "foo bar baz");
	set_line_text(2, "hello again");

	vimnav.x = 0;
	vimnav.y = 0;

	search_execute("hello", 1);
	ASSERT(search_active());

	/* Match on line 0 cols 0-4 */
	ASSERT_EQ(1, search_matched(0, 0));
	ASSERT_EQ(1, search_matched(4, 0));
	ASSERT_EQ(0, search_matched(5, 0));

	/* No match on line 1 */
	ASSERT_EQ(0, search_matched(0, 1));

	/* Match on line 2 cols 0-4 */
	ASSERT_EQ(1, search_matched(0, 2));
	ASSERT_EQ(1, search_matched(4, 2));
}

TEST(search_regex_pattern) {
	test_init();
	set_line_text(0, "abc 123 def 456");

	search_execute("[0-9]+", 1);
	ASSERT(search_active());

	/* "123" at cols 4-6 */
	ASSERT_EQ(0, search_matched(3, 0));
	ASSERT_EQ(1, search_matched(4, 0));
	ASSERT_EQ(1, search_matched(5, 0));
	ASSERT_EQ(1, search_matched(6, 0));
	ASSERT_EQ(0, search_matched(7, 0));

	/* "456" at cols 12-14 */
	ASSERT_EQ(0, search_matched(11, 0));
	ASSERT_EQ(1, search_matched(12, 0));
	ASSERT_EQ(1, search_matched(13, 0));
	ASSERT_EQ(1, search_matched(14, 0));
}

TEST(search_clear_removes_highlights) {
	test_init();
	set_line_text(0, "test match");

	search_execute("test", 1);
	ASSERT(search_active());
	ASSERT_EQ(1, search_matched(0, 0));

	search_clear();
	ASSERT_EQ(0, search_active());
	ASSERT_EQ(0, search_matched(0, 0));
}

TEST(search_cache_invalidation) {
	test_init();
	set_line_text(0, "word");

	search_execute("word", 1);
	ASSERT_EQ(1, search_matched(0, 0));

	/* Invalidate and re-check (should recompute) */
	search_invalidate_cache();
	ASSERT_EQ(1, search_matched(0, 0));
}

TEST(search_no_match) {
	test_init();
	set_line_text(0, "hello world");

	vimnav.x = 0;
	vimnav.y = 0;

	search_execute("nonexistent", 1);
	ASSERT(search_active());
	ASSERT_EQ(0, search_matched(0, 0));

	/* vimnav cursor shouldn't have moved (no match found) */
	ASSERT_EQ(0, vimnav.x);
	ASSERT_EQ(0, vimnav.y);
}

TEST(search_forward_moves_cursor) {
	test_init();
	set_line_text(0, "aaa");
	set_line_text(5, "hello target");

	vimnav.x = 0;
	vimnav.y = 0;

	search_execute("target", 1);
	/* Cursor should move to match on line 5, col 6 */
	ASSERT_EQ(6, vimnav.x);
}

TEST(search_backward) {
	test_init();
	set_line_text(2, "find me here");
	set_line_text(10, "starting point");

	vimnav.x = 0;
	vimnav.y = 10;

	search_execute("find", -1);
	ASSERT_EQ(0, vimnav.x);
	ASSERT_EQ(2, vimnav.y);
}

TEST(search_next_forward) {
	test_init();
	set_line_text(0, "match one");
	set_line_text(5, "match two");
	set_line_text(10, "match three");

	vimnav.x = 0;
	vimnav.y = 0;

	search_execute("match", 1);
	int first_y = vimnav.y;

	/* Go to next */
	search_next(1);
	int second_y = vimnav.y;

	/* Should have moved forward */
	ASSERT(second_y > first_y || (second_y == first_y && vimnav.x > 0));
}

TEST(search_next_backward) {
	test_init();
	set_line_text(0, "match one");
	set_line_text(5, "match two");
	set_line_text(10, "match three");

	vimnav.x = 0;
	vimnav.y = 10;

	search_execute("match", 1);

	/* N should go backward */
	search_next(-1);
	ASSERT(vimnav.y <= 10);
}

TEST(scroll_centering_basic) {
	int i, col;
	const char *text;

	test_init();
	/* Set up some history */
	term.histn = 100;
	term.histi = 99;

	for (i = 0; i < 100; i++) {
		/* Allocate history lines from a static pool */
		static Glyph histbufs[100][TEST_COLS];
		memset(histbufs[i], 0, sizeof(histbufs[i]));
		for (col = 0; col < TEST_COLS; col++)
			histbufs[i][col].u = ' ';
		term.hist[i] = histbufs[i];
	}

	/* Put "target" in history line 50 (absline 50) */
	text = "target here";
	col = 0;
	while (text[col] && col < TEST_COLS) {
		term.hist[50][col].u = text[col];
		col++;
	}

	vimnav.x = 0;
	vimnav.y = TEST_ROWS - 1;

	search_execute("target", 1);

	/* term.scr should be set to center the match */
	ASSERT(term.scr > 0);
}

TEST(search_empty_pattern) {
	test_init();
	set_line_text(0, "hello");

	search_execute("", 1);
	ASSERT_EQ(0, search_active());
}

TEST(search_multiple_matches_per_line) {
	test_init();
	set_line_text(0, "aa bb aa cc aa");

	search_execute("aa", 1);

	/* First "aa" at 0-1 */
	ASSERT_EQ(1, search_matched(0, 0));
	ASSERT_EQ(1, search_matched(1, 0));
	ASSERT_EQ(0, search_matched(2, 0));

	/* Second "aa" at 6-7 */
	ASSERT_EQ(1, search_matched(6, 0));
	ASSERT_EQ(1, search_matched(7, 0));

	/* Third "aa" at 12-13 */
	ASSERT_EQ(1, search_matched(12, 0));
	ASSERT_EQ(1, search_matched(13, 0));
}

/* --- Suite --- */

TEST_SUITE(search) {
	RUN_TEST(search_inactive_by_default);
	RUN_TEST(search_basic_forward);
	RUN_TEST(search_regex_pattern);
	RUN_TEST(search_clear_removes_highlights);
	RUN_TEST(search_cache_invalidation);
	RUN_TEST(search_no_match);
	RUN_TEST(search_forward_moves_cursor);
	RUN_TEST(search_backward);
	RUN_TEST(search_next_forward);
	RUN_TEST(search_next_backward);
	RUN_TEST(scroll_centering_basic);
	RUN_TEST(search_empty_pattern);
	RUN_TEST(search_multiple_matches_per_line);
}

int
main(void)
{
	printf("st search test suite\n");
	RUN_SUITE(search);
	return test_summary();
}
