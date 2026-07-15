/* See LICENSE for license details. */
/* Regression tests for the AeroSpace-managed window reveal handshake. */

#include "test.h"
#include "macos/reveal.h"

TEST(hidden_until_reveal_is_requested)
{
	ASSERT_EQ(0, macos_managed_reveal_ready(0, 0));
	ASSERT_EQ(0, macos_managed_reveal_ready(0, 1));
}

TEST(hidden_until_window_is_key)
{
	ASSERT_EQ(0, macos_managed_reveal_ready(1, 0));
}

TEST(reveals_after_request_and_native_focus)
{
	ASSERT_EQ(1, macos_managed_reveal_ready(1, 1));
}

TEST_SUITE(macos_reveal)
{
	RUN_TEST(hidden_until_reveal_is_requested);
	RUN_TEST(hidden_until_window_is_key);
	RUN_TEST(reveals_after_request_and_native_focus);
}

int
main(void)
{
	RUN_SUITE(macos_reveal);
	return test_summary();
}
