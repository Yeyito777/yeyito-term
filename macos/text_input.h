/* See LICENSE for license details. */

#ifndef ST_MACOS_TEXT_INPUT_H
#define ST_MACOS_TEXT_INPUT_H

#include <stddef.h>
#include <string.h>

static inline int
macos_is_legacy_ctrl_slash(unsigned long symbol, int shift, int option,
		int control, int command)
{
	/* AppKit leaves Ctrl+/ as '/', unlike the legacy terminal byte ^_. */
	return symbol == '/' && control && !shift && !option && !command;
}

static inline int
macos_is_em_dash_keystroke(int key_code, int shift, int option,
		int control, int command)
{
	/* Cocoa's standard Option+Shift+Minus text composition. */
	return key_code == 27 && shift && option && !control && !command;
}

/*
 * A key press while the keyboard layout may have a dead key pending, such as
 * an accent or a symbol prefix (Ctrl+\ then a -> ∀ in the user's ABC Symbols
 * layout). pending and pending_after say whether one was pending before and
 * after the press; composed is the layout's output for the press and plain
 * its output with nothing pending. Returns 1 when the press is fully handled.
 * Either way, the first *write_len bytes of composed are written first.
 */
static inline int
macos_dead_key_press(int pending, int pending_after, const char *composed,
		size_t composed_len, const char *plain, size_t plain_len,
		size_t *write_len)
{
	*write_len = 0;
	if (!pending && !pending_after)
		return 0;
	if (pending_after) {
		/* A dead key: write any terminator of one already pending, then wait. */
		*write_len = composed_len;
		return 1;
	}
	if (plain_len && composed_len >= plain_len &&
	    !memcmp(composed + composed_len - plain_len, plain, plain_len)) {
		/* Nothing composed: the terminator, then the key as usual. */
		*write_len = composed_len - plain_len;
		return 0;
	}
	*write_len = composed_len;
	return 1;
}

#endif /* ST_MACOS_TEXT_INPUT_H */
