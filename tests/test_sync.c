/* See LICENSE for license details. */
/* Regression tests for DEC 2026 synchronized-update watchdog state. */

#include "test.h"
#include "../sync.h"

TEST(set_starts_synchronized_update)
{
	SyncUpdate state = {0};
	struct timespec now = {.tv_sec = 10, .tv_nsec = 250000000};

	ASSERT_EQ(1, syncupdate_set(&state, 1, &now));
	ASSERT(state.active);
	ASSERT_EQ(1, state.generation);
	ASSERT_EQ(10, state.started.tv_sec);
	ASSERT_EQ(250000000, state.started.tv_nsec);
}

TEST(repeated_set_is_idempotent_and_does_not_extend_watchdog)
{
	SyncUpdate state = {0};
	struct timespec first = {.tv_sec = 1, .tv_nsec = 0};
	struct timespec later = {.tv_sec = 1, .tv_nsec = 100000000};

	syncupdate_set(&state, 1, &first);
	ASSERT_EQ(0, syncupdate_set(&state, 1, &later));
	ASSERT_EQ(1, state.generation);
	ASSERT_EQ(1, state.started.tv_sec);
	ASSERT_EQ(0, state.started.tv_nsec);
}

TEST(reset_ends_update_and_invalidates_old_watchdog)
{
	SyncUpdate state = {0};
	struct timespec now = {.tv_sec = 3, .tv_nsec = 0};

	syncupdate_set(&state, 1, &now);
	ASSERT_EQ(1, syncupdate_set(&state, 0, &now));
	ASSERT(!state.active);
	ASSERT_EQ(2, state.generation);
	ASSERT_EQ(0, syncupdate_set(&state, 0, &now));
}

TEST(watchdog_uses_monotonic_elapsed_time)
{
	SyncUpdate state = {0};
	struct timespec start = {.tv_sec = 7, .tv_nsec = 900000000};
	struct timespec before = {.tv_sec = 8, .tv_nsec = 49000000};
	struct timespec expired = {.tv_sec = 8, .tv_nsec = 50000000};

	syncupdate_set(&state, 1, &start);
	ASSERT(syncupdate_remaining(&state, &before, 150) > 0);
	ASSERT(syncupdate_remaining(&state, &before, 150) < 2);
	ASSERT_EQ(0, syncupdate_remaining(&state, &expired, 150));
}

TEST_SUITE(sync)
{
	RUN_TEST(set_starts_synchronized_update);
	RUN_TEST(repeated_set_is_idempotent_and_does_not_extend_watchdog);
	RUN_TEST(reset_ends_update_and_invalidates_old_watchdog);
	RUN_TEST(watchdog_uses_monotonic_elapsed_time);
}

int
main(void)
{
	RUN_SUITE(sync);
	return test_summary();
}
