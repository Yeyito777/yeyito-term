/* See LICENSE for license details. */
/* Regression tests for application mode recovery after SSH disconnects. */

#include "test.h"
#include "../st.h"
#include "../win.h"

TEST(clears_observed_ssh_leak)
{
	/* Captured from the softlocked local-shell window after SSH dropped. */
	unsigned int mode = 0xD8201;
	unsigned int restored = winmoderestore(mode);

	ASSERT(mode & MODE_KITTYKBD);
	ASSERT(mode & MODE_BRCKTPASTE);
	ASSERT(mode & MODE_PASTEEVENT);
	ASSERT(mode & MODE_MOUSEMANY);
	ASSERT(mode & MODE_MOUSESGR);
	ASSERT_EQ(MODE_VISIBLE, restored);
}

TEST(clears_every_application_controlled_mode)
{
	ASSERT_EQ(0, winmoderestore(MODE_APPRESET));
	ASSERT(MODE_APPRESET & MODE_SYNC);
}

TEST(preserves_window_owned_modes)
{
	unsigned int owned = MODE_VISIBLE | MODE_FOCUSED | MODE_BLINK |
	    MODE_FBLINK | MODE_NUMLOCK | MODE_ONSCREEN;

	ASSERT_EQ(owned, winmoderestore(owned | MODE_APPRESET));
}

TEST_SUITE(mode_reset)
{
	RUN_TEST(clears_observed_ssh_leak);
	RUN_TEST(clears_every_application_controlled_mode);
	RUN_TEST(preserves_window_owned_modes);
}

int
main(void)
{
	RUN_SUITE(mode_reset);
	return test_summary();
}
