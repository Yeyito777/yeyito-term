#ifndef MACOS_EMOJI_H
#define MACOS_EMOJI_H

#include <wchar.h>

#include "../st.h"

#define MACOS_TEXT_PRESENTATION_SELECTOR  0xfe0e
#define MACOS_EMOJI_PRESENTATION_SELECTOR 0xfe0f

int macos_apply_presentation_selector(Glyph *line, int columns,
		int cursor_column, int wrap_next, Rune selector);
int macos_expand_emoji_width(Glyph *line, int columns, int column);

#endif
