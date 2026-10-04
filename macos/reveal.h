/* See LICENSE for license details. */

#ifndef ST_MACOS_REVEAL_H
#define ST_MACOS_REVEAL_H

#include <stdlib.h>

/* This is a one-window launcher handshake, not a shell/session setting.
 * Consume it before ttynew() forks: otherwise an editor or nested st inherits
 * managed mode but has no launcher to send its SIGUSR1 reveal request. */
static inline int
macos_consume_managed_reveal(void)
{
	int managed = getenv("ST_AEROSPACE_MANAGED") != NULL;
	unsetenv("ST_AEROSPACE_MANAGED");
	return managed;
}

static inline int
macos_managed_reveal_ready(int reveal_requested, int is_key_window)
{
	return reveal_requested && is_key_window;
}

#endif /* ST_MACOS_REVEAL_H */
