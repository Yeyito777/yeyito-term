/* See LICENSE for license details. */
#ifndef SYNC_H
#define SYNC_H

#include <time.h>

/* Presentation state for DEC private mode 2026 (synchronized updates). */
typedef struct {
	int active;
	unsigned long generation;
	struct timespec started;
} SyncUpdate;

/* Apply boolean DECSET/DECRST semantics.  Repeated sets are idempotent and do
 * not extend the watchdog indefinitely.  Returns non-zero on a transition. */
static inline int
syncupdate_set(SyncUpdate *state, int set, const struct timespec *now)
{
	set = !!set;
	if (state->active == set)
		return 0;
	state->active = set;
	state->generation++;
	if (set && now)
		state->started = *now;
	return 1;
}

static inline double
syncupdate_remaining(const SyncUpdate *state, const struct timespec *now,
		unsigned int timeout)
{
	double elapsed;

	if (!state->active)
		return 0;
	elapsed = (now->tv_sec - state->started.tv_sec) * 1000.0 +
	    (now->tv_nsec - state->started.tv_nsec) / 1E6;
	return timeout - elapsed;
}

#endif /* SYNC_H */
