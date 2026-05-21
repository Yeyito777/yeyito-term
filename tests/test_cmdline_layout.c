/* See LICENSE for license details. */
/* Unit tests for command-line overlay geometry. */

#include "test.h"
#include "cmdline_layout.h"

TEST(exact_grid_bottom_row)
{
	CmdlineLayout l = cmdline_layout(1000, 804, 782, 800, 13, 4, 1);

	ASSERT_EQ(782, l.y);
	ASSERT_EQ(1000, l.width);
	ASSERT_EQ(22, l.height);      /* includes bottom border/padding */
	ASSERT_EQ(18, l.row_height);  /* the rendered terminal row itself */
	ASSERT_EQ(14, l.baseline);    /* top border + Xft ascent */
}

TEST(gpu_scaled_bottom_row)
{
	CmdlineLayout l = cmdline_layout(1003, 807, 785, 805, 13, 4, 1);

	ASSERT_EQ(785, l.y);
	ASSERT_EQ(1003, l.width);
	ASSERT_EQ(22, l.height);
	ASSERT_EQ(20, l.row_height);
	ASSERT_EQ(15, l.baseline);    /* vertically centered in actual GPU row */
}

TEST(clamps_invalid_inputs)
{
	CmdlineLayout l = cmdline_layout(80, 24, 30, 20, 10, 4, 1);

	ASSERT_EQ(24, l.y);
	ASSERT_EQ(80, l.width);
	ASSERT_EQ(1, l.height);
	ASSERT_EQ(1, l.row_height);
	ASSERT_EQ(0, l.baseline);
}

TEST(clamps_baseline_to_keep_descenders_visible)
{
	CmdlineLayout l = cmdline_layout(80, 24, 10, 12, 13, 4, 1);

	ASSERT_EQ(10, l.y);
	ASSERT_EQ(14, l.height);
	ASSERT_EQ(2, l.row_height);
	ASSERT_EQ(10, l.baseline); /* height - descent; avoids bottom clipping */
}

TEST_SUITE(cmdline_layout)
{
	RUN_TEST(exact_grid_bottom_row);
	RUN_TEST(gpu_scaled_bottom_row);
	RUN_TEST(clamps_invalid_inputs);
	RUN_TEST(clamps_baseline_to_keep_descenders_visible);
}

int
main(void)
{
	RUN_SUITE(cmdline_layout);
	return test_summary();
}
