/* See LICENSE for license details. */
/* Unit tests for OSC 779 CWD reporting feature */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

/* Track xsetcwd calls */
static struct {
	int calls;
	char last_cwd[4096];
} cwd_track;

/* Mock xsetcwd - this is what st.c calls when it receives OSC 779 */
void
xsetcwd(char *cwd)
{
	cwd_track.calls++;
	strncpy(cwd_track.last_cwd, cwd, sizeof(cwd_track.last_cwd) - 1);
	cwd_track.last_cwd[sizeof(cwd_track.last_cwd) - 1] = '\0';
}

static void
reset_track(void)
{
	memset(&cwd_track, 0, sizeof(cwd_track));
}

/*
 * Simulate the OSC 779 parsing logic from strhandle() in st.c.
 * This mirrors the exact code path: the OSC args are split by ';',
 * so args[0]="779" and args[1]=PATH.
 */
static void
simulate_osc779(int narg, char **args)
{
	int par;

	par = narg ? atoi(args[0]) : 0;

	if (par == 779) {
		if (narg > 1 && args[1][0] != '\0')
			xsetcwd(args[1]);
	}
}

/* Test: basic CWD reporting */
TEST(cwd_basic)
{
	char *args[] = { "779", "/home/user" };

	reset_track();
	simulate_osc779(2, args);

	ASSERT_EQ(1, cwd_track.calls);
	ASSERT_STR_EQ("/home/user", cwd_track.last_cwd);
}

/* Test: CWD with spaces in path */
TEST(cwd_spaces)
{
	char *args[] = { "779", "/home/user/my documents" };

	reset_track();
	simulate_osc779(2, args);

	ASSERT_EQ(1, cwd_track.calls);
	ASSERT_STR_EQ("/home/user/my documents", cwd_track.last_cwd);
}

/* Test: root path */
TEST(cwd_root)
{
	char *args[] = { "779", "/" };

	reset_track();
	simulate_osc779(2, args);

	ASSERT_EQ(1, cwd_track.calls);
	ASSERT_STR_EQ("/", cwd_track.last_cwd);
}

/* Test: empty path is ignored */
TEST(cwd_empty_ignored)
{
	char *args[] = { "779", "" };

	reset_track();
	simulate_osc779(2, args);

	ASSERT_EQ(0, cwd_track.calls);
}

/* Test: missing arg is ignored */
TEST(cwd_missing_arg)
{
	char *args[] = { "779" };

	reset_track();
	simulate_osc779(1, args);

	ASSERT_EQ(0, cwd_track.calls);
}

/* Test: different OSC number doesn't trigger CWD */
TEST(cwd_wrong_osc)
{
	char *args[] = { "778", "/home/user" };

	reset_track();
	simulate_osc779(2, args);

	ASSERT_EQ(0, cwd_track.calls);
}

/* Test: multiple updates, last one wins */
TEST(cwd_multiple_updates)
{
	char *args1[] = { "779", "/first" };
	char *args2[] = { "779", "/second" };

	reset_track();
	simulate_osc779(2, args1);
	simulate_osc779(2, args2);

	ASSERT_EQ(2, cwd_track.calls);
	ASSERT_STR_EQ("/second", cwd_track.last_cwd);
}

/* Test suite */
TEST_SUITE(cwd)
{
	RUN_TEST(cwd_basic);
	RUN_TEST(cwd_spaces);
	RUN_TEST(cwd_root);
	RUN_TEST(cwd_empty_ignored);
	RUN_TEST(cwd_missing_arg);
	RUN_TEST(cwd_wrong_osc);
	RUN_TEST(cwd_multiple_updates);
}

int
main(void)
{
	printf("st cwd test suite\n");
	printf("========================================\n");

	RUN_SUITE(cwd);

	return test_summary();
}
