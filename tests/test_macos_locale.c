#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "test.h"
#include "../macos/locale.h"

TEST(gui_environment_uses_utf8_widths)
{
	unsetenv("LC_ALL");
	unsetenv("LC_CTYPE");
	unsetenv("LANG");
	setlocale(LC_CTYPE, "C");

	ASSERT(wcwidth(0x1f600) < 0);
	ASSERT(macos_init_utf8_locale());
	ASSERT(strcmp(setlocale(LC_CTYPE, NULL), "C") != 0);
	ASSERT_NOT_NULL(getenv("LC_CTYPE"));
	ASSERT_STR_EQ(setlocale(LC_CTYPE, NULL), getenv("LC_CTYPE"));
	ASSERT_EQ(wcwidth(L'A'), 1);
	ASSERT_EQ(wcwidth(0x1f600), 2);
}

TEST_SUITE(macos_locale)
{
	RUN_TEST(gui_environment_uses_utf8_widths);
}

int
main(void)
{
	RUN_SUITE(macos_locale);
	return test_summary();
}
