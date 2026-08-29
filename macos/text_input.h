/* See LICENSE for license details. */

#ifndef ST_MACOS_TEXT_INPUT_H
#define ST_MACOS_TEXT_INPUT_H

static inline int
macos_is_em_dash_keystroke(int key_code, int shift, int option,
		int control, int command)
{
	/* Cocoa's standard Option+Shift+Minus text composition. */
	return key_code == 27 && shift && option && !control && !command;
}

#endif /* ST_MACOS_TEXT_INPUT_H */
