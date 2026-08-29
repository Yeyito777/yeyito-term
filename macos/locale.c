#include <locale.h>
#include <stdlib.h>
#include <string.h>

#include "locale.h"

int
macos_init_utf8_locale(void)
{
	const char *name = setlocale(LC_CTYPE, "");

	/*
	 * Launch Services does not normally provide LANG or LC_CTYPE to GUI
	 * applications.  Darwin then selects the C locale, whose wcwidth(3)
	 * cannot classify non-ASCII characters.  st treats those failures as
	 * single-cell glyphs, shrinking color emoji into half-width cells.
	 */
	if (!name || !strcmp(name, "C") || !strcmp(name, "POSIX")) {
		name = setlocale(LC_CTYPE, "UTF-8");
		if (!name)
			name = setlocale(LC_CTYPE, "en_US.UTF-8");
	}
	/*
	 * setlocale changes only this process.  Export the resolved character
	 * locale so the shell created after fork/exec does not fall back to the
	 * byte-oriented C locale and split UTF-8 input into invalid characters.
	 */
	if (name)
		setenv("LC_CTYPE", name, 1);
	return name != NULL;
}
