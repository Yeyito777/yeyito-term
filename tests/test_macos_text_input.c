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

TEST_SUITE(macos_text_input)
{
	RUN_TEST(recognizes_legacy_ctrl_slash);
	RUN_TEST(preserves_plain_slash_and_other_shortcuts);
	RUN_TEST(recognizes_cocoa_em_dash_keystroke);
	RUN_TEST(rejects_other_minus_modifiers);
	RUN_TEST(rejects_other_keys);
}

int
main(void)
{
	RUN_SUITE(macos_text_input);
	return test_summary();
}
