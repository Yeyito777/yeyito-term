/* See LICENSE for license details. */

#ifndef ST_MACOS_REVEAL_H
#define ST_MACOS_REVEAL_H

static inline int
macos_managed_reveal_ready(int reveal_requested, int is_key_window)
{
	return reveal_requested && is_key_window;
}

#endif /* ST_MACOS_REVEAL_H */
