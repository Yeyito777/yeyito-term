/* See LICENSE for license details. */
/* Regression tests for the AeroSpace-managed window reveal handshake. */

#include "test.h"
#include "macos/reveal.h"
#include <sys/wait.h>
#include <unistd.h>

TEST(standalone_launch_stays_unmanaged)
{
	unsetenv("ST_AEROSPACE_MANAGED");
	ASSERT_EQ(0, macos_consume_managed_reveal());
	ASSERT(getenv("ST_AEROSPACE_MANAGED") == NULL);
}

TEST(managed_launch_consumes_its_flag)
{
	ASSERT_EQ(0, setenv("ST_AEROSPACE_MANAGED", "1", 1));
	ASSERT_EQ(1, macos_consume_managed_reveal());
	ASSERT(getenv("ST_AEROSPACE_MANAGED") == NULL);
	ASSERT_EQ(0, macos_consume_managed_reveal());
}

TEST(pty_child_does_not_inherit_managed_reveal)
{
	ASSERT_EQ(0, setenv("ST_AEROSPACE_MANAGED", "1", 1));
	ASSERT_EQ(1, macos_consume_managed_reveal());
	pid_t child = fork();
	ASSERT(child >= 0);
	if (child == 0) {
		/* Exercise exec inheritance, as with st -> shell/Exocortex -> st. */
		execl("/bin/sh", "sh", "-c",
		    "test \"${ST_AEROSPACE_MANAGED+x}\" != x", (char *)NULL);
		_exit(127);
	}
	int status;
	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT(WIFEXITED(status));
	ASSERT_EQ(0, WEXITSTATUS(status));
}

TEST(explicit_nested_launcher_can_opt_in_again)
{
	ASSERT_EQ(0, setenv("ST_AEROSPACE_MANAGED", "1", 1));
	ASSERT_EQ(1, macos_consume_managed_reveal());
	ASSERT_EQ(0, setenv("ST_AEROSPACE_MANAGED", "1", 1));
	ASSERT_EQ(1, macos_consume_managed_reveal());
	ASSERT(getenv("ST_AEROSPACE_MANAGED") == NULL);
}

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
	RUN_TEST(standalone_launch_stays_unmanaged);
	RUN_TEST(managed_launch_consumes_its_flag);
	RUN_TEST(pty_child_does_not_inherit_managed_reveal);
	RUN_TEST(explicit_nested_launcher_can_opt_in_again);
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
