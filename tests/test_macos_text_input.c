/* See LICENSE for license details. */

#include "test.h"
#include "macos/text_input.h"

TEST(recognizes_legacy_ctrl_slash)
{
	ASSERT_EQ(1, macos_is_legacy_ctrl_slash('/', 0, 0, 1, 0));
}

TEST(preserves_plain_slash_and_other_shortcuts)
{
	ASSERT_EQ(0, macos_is_legacy_ctrl_slash('/', 0, 0, 0, 0));
	ASSERT_EQ(0, macos_is_legacy_ctrl_slash('/', 1, 0, 1, 0));
	ASSERT_EQ(0, macos_is_legacy_ctrl_slash('/', 0, 1, 1, 0));
	ASSERT_EQ(0, macos_is_legacy_ctrl_slash('/', 0, 0, 1, 1));
	ASSERT_EQ(0, macos_is_legacy_ctrl_slash('?', 1, 0, 1, 0));
	ASSERT_EQ(0, macos_is_legacy_ctrl_slash('a', 0, 0, 1, 0));
}

TEST(recognizes_cocoa_em_dash_keystroke)
{
	ASSERT_EQ(1, macos_is_em_dash_keystroke(27, 1, 1, 0, 0));
}

TEST(rejects_other_minus_modifiers)
{
	ASSERT_EQ(0, macos_is_em_dash_keystroke(27, 0, 1, 0, 0));
	ASSERT_EQ(0, macos_is_em_dash_keystroke(27, 1, 0, 0, 0));
	ASSERT_EQ(0, macos_is_em_dash_keystroke(27, 1, 1, 1, 0));
	ASSERT_EQ(0, macos_is_em_dash_keystroke(27, 1, 1, 0, 1));
}

TEST(rejects_other_keys)
{
	ASSERT_EQ(0, macos_is_em_dash_keystroke(24, 1, 1, 0, 0));
}

TEST(ordinary_keys_take_the_usual_path)
{
	size_t n = 9;
	ASSERT_EQ(0, macos_dead_key_press(0, 0, "a", 1, "a", 1, &n));
	ASSERT_EQ(0, n);
}

TEST(dead_key_waits_without_output)
{
	size_t n = 9;
	ASSERT_EQ(1, macos_dead_key_press(0, 1, "", 0, "", 0, &n));
	ASSERT_EQ(0, n);
}

TEST(composed_key_writes_symbol)
{
	size_t n = 0;
	ASSERT_EQ(1, macos_dead_key_press(1, 0, "\xe2\x88\x80", 3, "a", 1, &n));
	ASSERT_EQ(3, n);
}

TEST(uncomposed_key_after_prefix_types_normally)
{
	size_t n = 9;
	ASSERT_EQ(0, macos_dead_key_press(1, 0, "k", 1, "k", 1, &n));
	ASSERT_EQ(0, n);
	ASSERT_EQ(0, macos_dead_key_press(1, 0, "\033", 1, "\033", 1, &n));
	ASSERT_EQ(0, n);
}

TEST(uncomposed_key_after_accent_writes_terminator_first)
{
	size_t n = 0;
	ASSERT_EQ(0, macos_dead_key_press(1, 0, "\xc2\xb4k", 3, "k", 1, &n));
	ASSERT_EQ(2, n);
}

TEST(dead_key_after_dead_key_writes_terminator_and_waits)
{
	size_t n = 0;
	ASSERT_EQ(1, macos_dead_key_press(1, 1, "\xc2\xb4", 2, "", 0, &n));
	ASSERT_EQ(2, n);
}

TEST_SUITE(macos_text_input)
{
	RUN_TEST(recognizes_legacy_ctrl_slash);
	RUN_TEST(preserves_plain_slash_and_other_shortcuts);
	RUN_TEST(recognizes_cocoa_em_dash_keystroke);
	RUN_TEST(rejects_other_minus_modifiers);
	RUN_TEST(rejects_other_keys);
	RUN_TEST(ordinary_keys_take_the_usual_path);
	RUN_TEST(dead_key_waits_without_output);
	RUN_TEST(composed_key_writes_symbol);
	RUN_TEST(uncomposed_key_after_prefix_types_normally);
	RUN_TEST(uncomposed_key_after_accent_writes_terminator_first);
	RUN_TEST(dead_key_after_dead_key_writes_terminator_and_waits);
}

int
main(void)
{
	RUN_SUITE(macos_text_input);
	return test_summary();
}
