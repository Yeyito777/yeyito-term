#include "emoji.h"

int
macos_apply_presentation_selector(Glyph *line, int columns,
		int cursor_column, int wrap_next, Rune selector)
{
	int column;

	if (!line || columns <= 0 ||
	    (selector != MACOS_TEXT_PRESENTATION_SELECTOR &&
	     selector != MACOS_EMOJI_PRESENTATION_SELECTOR))
		return -1;

	column = cursor_column;
	if (!wrap_next)
		column--;
	if (column >= columns)
		column = columns - 1;
	if (column >= 0 && (line[column].mode & ATTR_WDUMMY))
		column--;
	if (column < 0)
		return -1;

	if (selector == MACOS_EMOJI_PRESENTATION_SELECTOR)
		line[column].mode |= ATTR_EMOJI;
	else
		line[column].mode &= ~ATTR_EMOJI;
	return column;
}

int
macos_expand_emoji_width(Glyph *line, int columns, int column)
{
	if (!line || column < 0 || column + 1 >= columns ||
	    (line[column].mode & ATTR_WIDE))
		return 0;

	line[column].mode |= ATTR_WIDE;
	line[column + 1].u = '\0';
	line[column + 1].mode = ATTR_WDUMMY;
	return 1;
}
