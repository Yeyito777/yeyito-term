/* See LICENSE for license details. */
/* Unit tests for vimnav module */

#include "test.h"
#include "mocks.h"
#include "../vimnav.h"

/* XK_Escape value */
#define XK_Escape 0xff1b

/* Test: entering vim nav mode */
TEST(vimnav_enter_sets_mode)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% prompt");  /* Set up a prompt line */
	term.c.x = 5;
	term.c.y = 10;
	vimnav.zsh_cursor = 3;  /* zsh reports cursor at position 3 (after prompt) */

	vimnav_enter();

	ASSERT(tisvimnav());
	/* x should be prompt_end (2, after "% ") + zsh_cursor (3) = 5 */
	ASSERT_EQ(5, vimnav.x);
	ASSERT_EQ(10, vimnav.y);

	vimnav_exit();
	mock_term_free();
}

/* Test: exiting vim nav mode */
TEST(vimnav_exit_clears_mode)
{
	mock_term_init(24, 80);

	vimnav_enter();
	ASSERT(tisvimnav());

	vimnav_exit();
	ASSERT(!tisvimnav());

	mock_term_free();
}

/* Test: vimnav doesn't enter on alt screen */
TEST(vimnav_no_enter_altscreen)
{
	mock_term_init(24, 80);
	term.mode = MODE_ALTSCREEN;

	vimnav_enter();
	ASSERT(!tisvimnav());

	mock_term_free();
}

/* Test: h key moves cursor left */
TEST(vimnav_h_moves_left)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	/* Start away from prompt line so h is handled internally */
	term.c.x = 0;
	term.c.y = 10;
	term.scr = 5;  /* Scrolled so we're not on prompt */

	vimnav_enter();
	vimnav.x = 5;
	vimnav.y = 5;
	vimnav.savedx = 5;

	int handled = vimnav_handle_key('h', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(4, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: j key moves cursor down */
TEST(vimnav_j_moves_down)
{
	mock_term_init(24, 80);
	mock_set_line(5, "line five");
	mock_set_line(6, "line six");

	term.c.x = 0;
	term.c.y = 20;  /* Prompt far down */

	vimnav_enter();
	vimnav.y = 5;
	vimnav.x = 0;

	int handled = vimnav_handle_key('j', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(6, vimnav.y);

	vimnav_exit();
	mock_term_free();
}

/* Test: k key moves cursor up */
TEST(vimnav_k_moves_up)
{
	mock_term_init(24, 80);
	mock_set_line(4, "line four");
	mock_set_line(5, "line five");

	term.c.x = 0;
	term.c.y = 20;

	vimnav_enter();
	vimnav.y = 5;
	vimnav.x = 0;

	int handled = vimnav_handle_key('k', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(4, vimnav.y);

	vimnav_exit();
	mock_term_free();
}

/* Test: l key moves cursor right */
TEST(vimnav_l_moves_right)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	term.c.x = 0;
	term.c.y = 10;
	term.scr = 5;

	vimnav_enter();
	vimnav.x = 3;
	vimnav.y = 5;
	vimnav.savedx = 3;

	int handled = vimnav_handle_key('l', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(4, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: 0 key moves to beginning of line */
TEST(vimnav_0_moves_bol)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	term.c.x = 0;
	term.c.y = 10;
	term.scr = 5;

	vimnav_enter();
	vimnav.x = 8;
	vimnav.y = 5;

	int handled = vimnav_handle_key('0', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: v toggles visual mode */
TEST(vimnav_v_toggles_visual)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	term.c.x = 0;
	term.c.y = 10;

	vimnav_enter();
	vimnav.x = 3;
	vimnav.y = 5;

	/* Enter visual mode */
	int handled = vimnav_handle_key('v', 0);
	ASSERT_EQ(1, handled);
	ASSERT(mock_state.selstart_calls > 0);

	/* Exit visual mode */
	mock_reset();
	handled = vimnav_handle_key('v', 0);
	ASSERT_EQ(1, handled);
	ASSERT(mock_state.selclear_calls > 0);

	vimnav_exit();
	mock_term_free();
}

/* Test: V toggles visual line mode */
TEST(vimnav_V_toggles_visual_line)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	term.c.x = 0;
	term.c.y = 10;

	vimnav_enter();
	vimnav.x = 3;
	vimnav.y = 5;

	/* Enter visual line mode */
	int handled = vimnav_handle_key('V', 0);
	ASSERT_EQ(1, handled);
	ASSERT(mock_state.selstart_calls > 0);

	vimnav_exit();
	mock_term_free();
}

/* Test: Escape clears visual selection */
TEST(vimnav_escape_clears_visual)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	term.c.x = 0;
	term.c.y = 10;

	vimnav_enter();
	vimnav.x = 3;
	vimnav.y = 5;

	/* Enter visual mode */
	vimnav_handle_key('v', 0);

	/* Press escape */
	mock_reset();
	int handled = vimnav_handle_key(XK_Escape, 0);
	ASSERT_EQ(1, handled);
	ASSERT(mock_state.selclear_calls > 0);

	vimnav_exit();
	mock_term_free();
}

/* Test: tisvimnav returns correct state */
TEST(tisvimnav_returns_correct_state)
{
	mock_term_init(24, 80);

	ASSERT(!tisvimnav());

	vimnav_enter();
	ASSERT(tisvimnav());

	vimnav_exit();
	ASSERT(!tisvimnav());

	mock_term_free();
}

/* Test: Ctrl+u scrolls up half page */
TEST(vimnav_ctrl_u_scrolls_up)
{
	mock_term_init(24, 80);

	/* Setup some content */
	for (int i = 0; i < 24; i++) {
		mock_set_line(i, "line content");
	}

	term.c.x = 0;
	term.c.y = 20;

	vimnav_enter();
	vimnav.y = 12;

	mock_reset();
	int handled = vimnav_handle_key('u', 4);  /* 4 = ControlMask */

	ASSERT_EQ(1, handled);

	vimnav_exit();
	mock_term_free();
}

/* Test: v on prompt line passes to zsh, movement passes to zsh for cursor sync */
TEST(vimnav_visual_mode_prompt_line_movement)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% echo hello world");

	/* Setup: cursor on prompt line, not scrolled */
	term.c.x = 10;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 8;  /* Cursor at position 8 after prompt */

	vimnav_enter();
	/* x = prompt_end (2) + zsh_cursor (8) = 10 */
	ASSERT_EQ(10, vimnav.x);
	ASSERT_EQ(10, vimnav.y);

	/* 'v' on prompt line passes to zsh (zsh renders selection) */
	mock_reset();
	int handled = vimnav_handle_key('v', 0);
	ASSERT_EQ(0, handled);  /* Pass through to zsh */

	/* 'l' on prompt line should pass to zsh (cursor sync) */
	mock_reset();
	handled = vimnav_handle_key('l', 0);
	ASSERT_EQ(0, handled);  /* Pass through to zsh */

	/* 'h' on prompt line should pass to zsh (cursor sync) */
	mock_reset();
	handled = vimnav_handle_key('h', 0);
	ASSERT_EQ(0, handled);  /* Pass through to zsh */

	vimnav_exit();
	mock_term_free();
}

/* Test: normal mode on prompt line passes h/l to zsh */
TEST(vimnav_normal_mode_prompt_line_passes_to_zsh)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% echo hello world");

	/* Setup: cursor on prompt line, not scrolled */
	term.c.x = 10;
	term.c.y = 10;
	term.scr = 0;

	vimnav_enter();

	/* In normal mode, 'l' on prompt line should pass to zsh (return 0) */
	mock_reset();
	int handled = vimnav_handle_key('l', 0);
	ASSERT_EQ(0, handled);  /* Pass through to zsh */

	/* Cursor position updated but no selection calls */
	ASSERT_EQ(0, mock_state.selextend_calls);

	vimnav_exit();
	mock_term_free();
}

/* Test: zsh cursor sync updates vimnav cursor on prompt line */
TEST(vimnav_zsh_cursor_sync)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% prompt text");

	term.c.x = 5;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	/* Initially x = prompt_end (2) + zsh_cursor (0) = 2 */
	ASSERT_EQ(2, vimnav.x);

	/* zsh reports new cursor position */
	vimnav_set_zsh_cursor(5);
	/* x should now be prompt_end (2) + 5 = 7 */
	ASSERT_EQ(7, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: destructive operations (x, d, c) are disabled in history but pass through on prompt */
TEST(vimnav_destructive_ops_disabled)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	/* Test in history (not on prompt line) - should be disabled */
	term.c.x = 0;
	term.c.y = 10;
	term.scr = 5;  /* Scrolled, so vimnav.y != term.c.y */

	vimnav_enter();
	vimnav.x = 5;
	vimnav.y = 5;

	/* 'x' should be consumed but do nothing in history */
	int handled = vimnav_handle_key('x', 0);
	ASSERT_EQ(1, handled);  /* Consumed */
	ASSERT_EQ(5, vimnav.x);  /* Position unchanged */

	/* 'd' should be consumed but do nothing in history */
	handled = vimnav_handle_key('d', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(5, vimnav.x);

	/* 'c' should be consumed but do nothing in history */
	handled = vimnav_handle_key('c', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(5, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: destructive operations pass through to zsh on prompt line */
TEST(vimnav_destructive_ops_prompt_passthrough)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% echo hello");

	/* On prompt line - should pass through to zsh */
	term.c.x = 5;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 3;

	vimnav_enter();

	/* 'x' on prompt line should pass to zsh */
	int handled = vimnav_handle_key('x', 0);
	ASSERT_EQ(0, handled);  /* Pass through */

	/* 'd' on prompt line should pass to zsh */
	handled = vimnav_handle_key('d', 0);
	ASSERT_EQ(0, handled);  /* Pass through */

	/* 'c' on prompt line should pass to zsh */
	handled = vimnav_handle_key('c', 0);
	ASSERT_EQ(0, handled);  /* Pass through */

	vimnav_exit();
	mock_term_free();
}

/* Test: visual mode inherited from zsh on vimnav_enter */
TEST(vimnav_inherits_zsh_visual)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% echo hello");

	term.c.x = 10;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 5;
	vimnav.zsh_visual = 1;
	vimnav.zsh_visual_anchor = 2;
	vimnav.zsh_visual_line = 0;

	vimnav_enter();

	/* Should be in visual mode */
	ASSERT(vimnav.mode != 1);  /* Not VIMNAV_NORMAL (1) */
	/* Anchor should be set from zsh_visual_anchor */
	/* anchor_x = prompt_end (2) + zsh_visual_anchor (2) = 4 */
	ASSERT_EQ(4, vimnav.anchor_x);

	vimnav_exit();
	mock_term_free();
}

/* Test: visual mode handoff when pressing k to move off prompt line */
TEST(vimnav_visual_handoff_on_k)
{
	mock_term_init(24, 80);
	mock_set_line(9, "previous line");
	mock_set_line(10, "% echo hello");

	/* Start on prompt line */
	term.c.x = 10;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 5;
	vimnav.zsh_visual = 0;  /* Not in visual mode yet */

	vimnav_enter();
	ASSERT_EQ(1, vimnav.mode);  /* VIMNAV_NORMAL */

	/* Simulate zsh entering visual mode and reporting it */
	vimnav.zsh_visual = 1;
	vimnav.zsh_visual_anchor = 3;
	vimnav.zsh_visual_line = 0;

	/* Press k to move up - should trigger handoff */
	mock_reset();
	int handled = vimnav_handle_key('k', 0);
	ASSERT_EQ(1, handled);

	/* Should now be in visual mode with anchor on prompt line */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(9, vimnav.y);  /* Moved up to line 9 */
	/* anchor_x = prompt_end (2) + zsh_visual_anchor (3) = 5 */
	ASSERT_EQ(5, vimnav.anchor_x);
	ASSERT(mock_state.selstart_calls > 0);  /* Selection started */

	vimnav_exit();
	mock_term_free();
}

/* Test suite */
TEST_SUITE(vimnav)
{
	RUN_TEST(vimnav_enter_sets_mode);
	RUN_TEST(vimnav_exit_clears_mode);
	RUN_TEST(vimnav_no_enter_altscreen);
	RUN_TEST(vimnav_h_moves_left);
	RUN_TEST(vimnav_j_moves_down);
	RUN_TEST(vimnav_k_moves_up);
	RUN_TEST(vimnav_l_moves_right);
	RUN_TEST(vimnav_0_moves_bol);
	RUN_TEST(vimnav_v_toggles_visual);
	RUN_TEST(vimnav_V_toggles_visual_line);
	RUN_TEST(vimnav_escape_clears_visual);
	RUN_TEST(tisvimnav_returns_correct_state);
	RUN_TEST(vimnav_ctrl_u_scrolls_up);
	RUN_TEST(vimnav_visual_mode_prompt_line_movement);
	RUN_TEST(vimnav_normal_mode_prompt_line_passes_to_zsh);
	RUN_TEST(vimnav_zsh_cursor_sync);
	RUN_TEST(vimnav_destructive_ops_disabled);
	RUN_TEST(vimnav_destructive_ops_prompt_passthrough);
	RUN_TEST(vimnav_inherits_zsh_visual);
	RUN_TEST(vimnav_visual_handoff_on_k);
}

int
main(void)
{
	printf("st test suite\n");
	printf("========================================\n");

	RUN_SUITE(vimnav);

	return test_summary();
}
