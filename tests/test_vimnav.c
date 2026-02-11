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
	mock_set_line(10, "prompt line");

	/* Test in history (scrolled up) - should snap back to prompt and pass to zsh */
	term.c.x = 5;
	term.c.y = 10;
	term.scr = 5;  /* Scrolled up */

	vimnav_enter();
	vimnav.x = 5;
	vimnav.y = 5;  /* Viewing history */

	/* 'x' should snap back to prompt and pass to zsh */
	int handled = vimnav_handle_key('x', 0);
	ASSERT_EQ(0, handled);  /* Passed to zsh */
	ASSERT_EQ(0, term.scr);  /* Scrolled back to bottom */
	ASSERT_EQ(10, vimnav.y);  /* At prompt line */

	/* Reset for next test */
	term.scr = 5;
	vimnav.y = 5;

	/* 'd' should snap back to prompt and pass to zsh */
	handled = vimnav_handle_key('d', 0);
	ASSERT_EQ(0, handled);
	ASSERT_EQ(0, term.scr);
	ASSERT_EQ(10, vimnav.y);

	/* Reset for next test */
	term.scr = 5;
	vimnav.y = 5;

	/* 'c' should snap back to prompt and pass to zsh */
	handled = vimnav_handle_key('c', 0);
	ASSERT_EQ(0, handled);
	ASSERT_EQ(0, term.scr);
	ASSERT_EQ(10, vimnav.y);

	vimnav_exit();
	mock_term_free();
}

/* Test: J/K (Shift+j/k) snap to prompt and pass to zsh */
TEST(vimnav_JK_snap_to_prompt)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");
	mock_set_line(10, "prompt line");

	term.c.x = 5;
	term.c.y = 10;
	term.scr = 5;  /* Scrolled up */

	vimnav_enter();
	vimnav.x = 5;
	vimnav.y = 5;  /* Viewing history */

	/* 'J' should snap back to prompt and pass to zsh */
	int handled = vimnav_handle_key('J', 0);
	ASSERT_EQ(0, handled);  /* Passed to zsh */
	ASSERT_EQ(0, term.scr);  /* Scrolled back to bottom */
	ASSERT_EQ(10, vimnav.y);  /* At prompt line */

	/* Reset for K test */
	term.scr = 5;
	vimnav.y = 5;

	/* 'K' should snap back to prompt and pass to zsh */
	handled = vimnav_handle_key('K', 0);
	ASSERT_EQ(0, handled);
	ASSERT_EQ(0, term.scr);
	ASSERT_EQ(10, vimnav.y);

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

/* Test: all editing keys snap back to prompt from history */
TEST(vimnav_editing_keys_snap_to_prompt)
{
	mock_term_init(24, 80);
	mock_set_line(5, "history line");
	mock_set_line(10, "% prompt");

	term.c.x = 5;
	term.c.y = 10;

	/* Test various editing keys that should snap back
	 * Note: 'a' and 'i' are not included because they start text object
	 * sequences when in history (e.g., 'iw' for inner word) */
	char editing_keys[] = {'D', 'C', 's', 'S', 'r', 'R', 'A', 'I', 'o', 'O', 'u', '.'};
	int num_keys = sizeof(editing_keys) / sizeof(editing_keys[0]);
	int i;

	for (i = 0; i < num_keys; i++) {
		term.scr = 5;  /* Scrolled up */
		vimnav_enter();
		vimnav.y = 5;  /* In history */

		int handled = vimnav_handle_key(editing_keys[i], 0);
		ASSERT_EQ(0, handled);  /* Passed to zsh */
		ASSERT_EQ(0, term.scr);  /* Scrolled back */
		ASSERT_EQ(10, vimnav.y);  /* At prompt */

		vimnav_exit();
	}

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

/* Test: Escape clears zsh-handled visual mode on prompt line */
TEST(vimnav_escape_clears_zsh_visual)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% echo hello");

	/* On prompt line, zsh handling visual mode */
	term.c.x = 10;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 5;
	vimnav.zsh_visual = 0;  /* Reset from previous tests */

	vimnav_enter();
	ASSERT_EQ(1, vimnav.mode);  /* VIMNAV_NORMAL */

	/* Simulate zsh entering visual mode (as if user pressed v, passed to zsh) */
	vimnav.zsh_visual = 1;
	vimnav.zsh_visual_anchor = 2;
	vimnav.zsh_visual_line = 0;

	/* Press Escape - should clear zsh visual state */
	mock_reset();
	int handled = vimnav_handle_key(XK_Escape, 0);
	ASSERT_EQ(1, handled);  /* Consumed */
	ASSERT_EQ(0, vimnav.zsh_visual);  /* zsh visual cleared */
	ASSERT(mock_state.selclear_calls > 0);  /* Selection cleared */

	vimnav_exit();
	mock_term_free();
}

/* Test: returning to prompt from history syncs cursor to zsh */
TEST(vimnav_cursor_sync_on_return_to_prompt)
{
	mock_term_init(24, 80);
	mock_set_line(9, "previous output");
	mock_set_line(10, "% echo hello");

	/* Start on prompt line */
	term.c.x = 10;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 5;  /* zsh cursor at position 5 */
	vimnav.zsh_visual = 0;

	vimnav_enter();
	/* x = prompt_end (2) + zsh_cursor (5) = 7 */
	ASSERT_EQ(7, vimnav.x);
	ASSERT_EQ(10, vimnav.y);

	/* Move up to history (line 9) */
	vimnav_handle_key('k', 0);
	ASSERT_EQ(9, vimnav.y);

	/* Move cursor in history with h (st handles this, not zsh) */
	vimnav.x = 3;  /* Simulate moving cursor in history */
	vimnav.savedx = 3;

	/* Move back down to prompt - should sync to zsh cursor, not savedx */
	vimnav_handle_key('j', 0);
	ASSERT_EQ(10, vimnav.y);
	/* Should be synced to zsh cursor: prompt_end (2) + zsh_cursor (5) = 7 */
	ASSERT_EQ(7, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: repeated j in prompt space keeps cursor synced to zsh (e.g., after Ctrl+L) */
TEST(vimnav_cursor_sync_repeated_j_in_prompt_space)
{
	mock_term_init(24, 80);
	/* After Ctrl+L: prompt at row 0, history in scrollback */
	mock_set_line(0, "% ");  /* Empty prompt, no user input */
	mock_set_hist(0, "some old output");
	mock_set_hist(1, "more old output");

	term.c.x = 2;
	term.c.y = 0;  /* Prompt at top of screen (after Ctrl+L) */
	term.scr = 0;
	vimnav.zsh_cursor = 0;  /* No input typed */
	vimnav.zsh_visual = 0;

	vimnav_enter();
	/* x = prompt_end (2) + zsh_cursor (0) = 2 */
	ASSERT_EQ(2, vimnav.x);
	ASSERT_EQ(0, vimnav.y);

	/* Scroll up into history - cursor one row above prompt */
	term.scr = 1;
	vimnav.y = 0;
	vimnav.x = 3;
	vimnav.savedx = 3;

	/* Press j: enters prompt space (prompt is at screen row scr+c.y = 1).
	 * First transition into prompt space - should sync. */
	vimnav_handle_key('j', 0);
	ASSERT_EQ(1, vimnav.y);  /* Moved to prompt row */
	ASSERT_EQ(2, vimnav.x);  /* Synced: prompt_end (2) + zsh_cursor (0) */

	/* Press j again: already in prompt space, term.scr=1 > 0 so it scrolls
	 * down by 1. Cursor stays in prompt space. Must still sync to zsh cursor,
	 * NOT fall to linelen-1. */
	ASSERT(term.scr > 0);
	vimnav_handle_key('j', 0);
	ASSERT_EQ(2, vimnav.x);  /* Still synced to zsh cursor */

	vimnav_exit();
	mock_term_free();
}

/* Test: visual mode prevents cursor sync until selection done */
TEST(vimnav_visual_mode_defers_cursor_sync)
{
	mock_term_init(24, 80);
	mock_set_line(9, "previous output");
	mock_set_line(10, "% echo hello");

	term.c.x = 10;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 5;
	vimnav.zsh_visual = 0;

	vimnav_enter();
	ASSERT_EQ(7, vimnav.x);  /* prompt_end (2) + 5 */

	/* Move up and enter visual mode */
	vimnav_handle_key('k', 0);
	ASSERT_EQ(9, vimnav.y);

	/* Enter visual mode in history */
	vimnav_handle_key('v', 0);
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */

	/* Move cursor in history */
	vimnav.x = 3;
	vimnav.savedx = 3;

	/* Move back to prompt in visual mode - should NOT sync (preserves selection) */
	vimnav_handle_key('j', 0);
	ASSERT_EQ(10, vimnav.y);
	ASSERT_EQ(3, vimnav.x);  /* savedx, not zsh cursor */

	/* Exit visual mode with Escape - NOW should sync */
	mock_reset();
	vimnav_handle_key(XK_Escape, 0);
	ASSERT_EQ(1, vimnav.mode);  /* VIMNAV_NORMAL */
	ASSERT_EQ(7, vimnav.x);  /* Synced to zsh cursor */

	vimnav_exit();
	mock_term_free();
}

/* Test: y clears zsh visual mode on prompt line so k doesn't inherit stale selection */
TEST(vimnav_yank_clears_zsh_visual)
{
	mock_term_init(24, 80);
	mock_set_line(9, "previous output");
	mock_set_line(10, "% echo hello");

	/* On prompt line */
	term.c.x = 10;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 5;
	vimnav.zsh_visual = 0;

	vimnav_enter();
	ASSERT_EQ(1, vimnav.mode);  /* VIMNAV_NORMAL */

	/* Simulate zsh entering visual mode (user pressed v, passed to zsh) */
	vimnav.zsh_visual = 1;
	vimnav.zsh_visual_anchor = 2;
	vimnav.zsh_visual_line = 0;

	/* Simulate zsh creating a selection that st renders */
	selstart(4, 10, 0);  /* anchor_x = prompt_end (2) + anchor (2) = 4 */
	selextend(7, 10, 1, 0);

	/* Press y to yank - should clear zsh_visual */
	mock_reset();
	int handled = vimnav_handle_key('y', 0);
	ASSERT_EQ(1, handled);  /* Consumed */
	ASSERT_EQ(0, vimnav.zsh_visual);  /* zsh visual cleared by yank */
	ASSERT(mock_state.selclear_calls > 0);  /* Selection cleared */

	/* Now press k to move up - should NOT inherit stale selection */
	mock_reset();
	handled = vimnav_handle_key('k', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(9, vimnav.y);  /* Moved up */
	ASSERT_EQ(1, vimnav.mode);  /* Still VIMNAV_NORMAL, not VIMNAV_VISUAL */

	vimnav_exit();
	mock_term_free();
}

/* Test: v toggle off notifies zsh to exit visual mode (sends Escape) */
TEST(vimnav_v_toggle_off_notifies_zsh)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	term.c.x = 0;
	term.c.y = 10;
	vimnav.zsh_visual = 0;  /* Start without zsh visual */

	vimnav_enter();

	/* Simulate scrolling up into history after entering nav mode */
	term.scr = 5;
	vimnav.x = 3;
	vimnav.y = 5;

	/* Enter visual mode in history with v */
	mock_reset();
	vimnav_handle_key('v', 0);
	ASSERT(mock_state.selstart_calls > 0);

	/* Now toggle off visual mode with v - should notify zsh */
	mock_reset();
	vimnav.zsh_visual = 1;  /* Simulate zsh still thinking it's in visual mode */
	vimnav_handle_key('v', 0);
	ASSERT(mock_state.selclear_calls > 0);
	/* Should have sent Escape to zsh */
	ASSERT(mock_state.ttywrite_calls > 0);
	ASSERT_EQ('\033', mock_state.ttywrite_buf[0]);
	ASSERT_EQ(0, vimnav.zsh_visual);  /* zsh_visual flag cleared */

	vimnav_exit();
	mock_term_free();
}

/* Test: V toggle off notifies zsh to exit visual mode (sends Escape) */
TEST(vimnav_V_toggle_off_notifies_zsh)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	term.c.x = 0;
	term.c.y = 10;
	vimnav.zsh_visual = 0;  /* Start without zsh visual */

	vimnav_enter();

	/* Simulate scrolling up into history after entering nav mode */
	term.scr = 5;
	vimnav.x = 3;
	vimnav.y = 5;

	/* Enter visual line mode with V */
	mock_reset();
	vimnav_handle_key('V', 0);
	ASSERT(mock_state.selstart_calls > 0);

	/* Now toggle off visual line mode with V - should notify zsh */
	mock_reset();
	vimnav.zsh_visual = 1;  /* Simulate zsh still thinking it's in visual mode */
	vimnav_handle_key('V', 0);
	ASSERT(mock_state.selclear_calls > 0);
	/* Should have sent Escape to zsh */
	ASSERT(mock_state.ttywrite_calls > 0);
	ASSERT_EQ('\033', mock_state.ttywrite_buf[0]);
	ASSERT_EQ(0, vimnav.zsh_visual);  /* zsh_visual flag cleared */

	vimnav_exit();
	mock_term_free();
}

/* Test: e key moves to end of word */
TEST(vimnav_e_moves_to_word_end)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world test");

	/* Prompt at row 23, not scrolled, so row 5 is in history */
	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 0;  /* Start at 'h' of "hello" */
	vimnav.y = 5;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('e', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(4, vimnav.x);  /* End of "hello" (index 4) */

	/* Press e again to go to end of "world" */
	handled = vimnav_handle_key('e', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(10, vimnav.x);  /* End of "world" (index 10) */

	vimnav_exit();
	mock_term_free();
}

/* Test: W key moves to start of next WORD (whitespace-delimited) */
TEST(vimnav_W_moves_to_next_WORD)
{
	mock_term_init(24, 80);
	mock_set_line(5, "foo.bar baz-qux end");

	/* Prompt at row 23, not scrolled, so row 5 is in history */
	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 0;  /* Start at 'f' of "foo.bar" */
	vimnav.y = 5;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('W', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(8, vimnav.x);  /* Start of "baz-qux" (index 8) */

	/* Press W again to go to "end" */
	handled = vimnav_handle_key('W', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(16, vimnav.x);  /* Start of "end" (index 16) */

	vimnav_exit();
	mock_term_free();
}

/* Test: B key moves to start of previous WORD (whitespace-delimited) */
TEST(vimnav_B_moves_to_prev_WORD)
{
	mock_term_init(24, 80);
	mock_set_line(5, "foo.bar baz-qux end");

	/* Prompt at row 23, not scrolled, so row 5 is in history */
	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 18;  /* At 'd' of "end" */
	vimnav.y = 5;
	vimnav.savedx = 18;

	int handled = vimnav_handle_key('B', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(16, vimnav.x);  /* Start of "end" (index 16) - B from within word goes to word start */

	/* Press B again to go to "baz-qux" */
	handled = vimnav_handle_key('B', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(8, vimnav.x);  /* Start of "baz-qux" (index 8) */

	/* Press B again to go to "foo.bar" */
	handled = vimnav_handle_key('B', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, vimnav.x);  /* Start of "foo.bar" (index 0) */

	vimnav_exit();
	mock_term_free();
}

/* Test: E key moves to end of WORD (whitespace-delimited) */
TEST(vimnav_E_moves_to_WORD_end)
{
	mock_term_init(24, 80);
	mock_set_line(5, "foo.bar baz-qux end");

	/* Prompt at row 23, not scrolled, so row 5 is in history */
	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 0;  /* Start at 'f' of "foo.bar" */
	vimnav.y = 5;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('E', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(6, vimnav.x);  /* End of "foo.bar" (index 6) */

	/* Press E again to go to end of "baz-qux" */
	handled = vimnav_handle_key('E', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(14, vimnav.x);  /* End of "baz-qux" (index 14) */

	vimnav_exit();
	mock_term_free();
}

/* Test: e, E, W, B pass through to zsh on prompt line */
TEST(vimnav_eEWB_prompt_passthrough)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% echo hello world");

	/* On prompt line - should pass through to zsh */
	term.c.x = 5;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 3;

	vimnav_enter();

	/* All these should pass through on prompt line */
	ASSERT_EQ(0, vimnav_handle_key('e', 0));
	ASSERT_EQ(0, vimnav_handle_key('E', 0));
	ASSERT_EQ(0, vimnav_handle_key('W', 0));
	ASSERT_EQ(0, vimnav_handle_key('B', 0));

	vimnav_exit();
	mock_term_free();
}

/* Test: h/l keys work on history lines even when prompt ends with just "% " (trailing space stripped) */
TEST(vimnav_hl_works_on_history_with_empty_prompt)
{
	mock_term_init(24, 80);

	/* Previous prompt with command - has "% " followed by content */
	mock_set_line(8, "[user@host]% wc -l *");
	/* Command output */
	mock_set_line(9, "    100 total");
	/* Current prompt: ends with "% " - tlinelen strips trailing space, leaving just "%" */
	mock_set_line(10, "[user@host]% ");

	/* Cursor on current prompt line */
	term.c.x = 13;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	ASSERT_EQ(10, vimnav.y);

	/* Move up to output line (line 9) */
	vimnav_handle_key('k', 0);
	ASSERT_EQ(9, vimnav.y);

	/* h/l should be handled by st (return 1), NOT passed to zsh (return 0) */
	/* This was the bug: vimnav_is_prompt_space() incorrectly returned true */
	/* because it found the OLD prompt on line 8 as prompt_start */
	vimnav.x = 5;
	vimnav.savedx = 5;

	int handled = vimnav_handle_key('h', 0);
	ASSERT_EQ(1, handled);  /* st handles it */
	ASSERT_EQ(4, vimnav.x);  /* Cursor moved left */

	handled = vimnav_handle_key('l', 0);
	ASSERT_EQ(1, handled);  /* st handles it */
	ASSERT_EQ(5, vimnav.x);  /* Cursor moved right */

	vimnav_exit();
	mock_term_free();
}

/* Test: viw selects inner word */
TEST(vimnav_viw_selects_inner_word)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world test");

	/* Prompt at row 23, not scrolled, so row 5 is in history */
	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 7;  /* Cursor on 'o' of "world" */
	vimnav.y = 5;
	vimnav.savedx = 7;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */

	/* Press 'i' then 'w' for inner word */
	int handled = vimnav_handle_key('i', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ('i', vimnav.pending_textobj);

	handled = vimnav_handle_key('w', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, vimnav.pending_textobj);  /* Cleared */

	/* Should be in visual mode with word "world" selected (6-10) */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(6, vimnav.anchor_x);  /* Start of "world" */
	ASSERT_EQ(10, vimnav.x);  /* End of "world" */

	vimnav_exit();
	mock_term_free();
}

/* Test: vaw selects around word (includes trailing space) */
TEST(vimnav_vaw_selects_around_word)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world test");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 7;  /* Cursor on 'o' of "world" */
	vimnav.y = 5;
	vimnav.savedx = 7;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'a' then 'w' for around word */
	vimnav_handle_key('a', 0);
	int handled = vimnav_handle_key('w', 0);
	ASSERT_EQ(1, handled);

	/* Should include trailing space: "world " (6-11) */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(6, vimnav.anchor_x);  /* Start of "world" */
	ASSERT_EQ(11, vimnav.x);  /* Includes trailing space */

	vimnav_exit();
	mock_term_free();
}

/* Test: viW selects inner WORD (whitespace-delimited) */
TEST(vimnav_viW_selects_inner_WORD)
{
	mock_term_init(24, 80);
	mock_set_line(5, "foo.bar baz-qux end");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 2;  /* Cursor on 'o' of "foo.bar" */
	vimnav.y = 5;
	vimnav.savedx = 2;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then 'W' for inner WORD */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('W', 0);
	ASSERT_EQ(1, handled);

	/* Should select "foo.bar" (0-6) */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(0, vimnav.anchor_x);
	ASSERT_EQ(6, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: vi" selects inner quotes */
TEST(vimnav_vi_quote_selects_inner_quotes)
{
	mock_term_init(24, 80);
	mock_set_line(5, "echo \"hello world\" done");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 10;  /* Cursor on 'l' of "hello" inside quotes */
	vimnav.y = 5;
	vimnav.savedx = 10;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then '"' for inner quotes */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('"', 0);
	ASSERT_EQ(1, handled);

	/* Should select "hello world" (6-16), not including quotes */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(6, vimnav.anchor_x);  /* After opening quote */
	ASSERT_EQ(16, vimnav.x);  /* Before closing quote */

	vimnav_exit();
	mock_term_free();
}

/* Test: va" selects around quotes (includes quotes) */
TEST(vimnav_va_quote_selects_around_quotes)
{
	mock_term_init(24, 80);
	mock_set_line(5, "echo \"hello world\" done");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 10;  /* Cursor inside quotes */
	vimnav.y = 5;
	vimnav.savedx = 10;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'a' then '"' for around quotes */
	vimnav_handle_key('a', 0);
	int handled = vimnav_handle_key('"', 0);
	ASSERT_EQ(1, handled);

	/* Should select "\"hello world\"" (5-17), including quotes */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(5, vimnav.anchor_x);  /* Opening quote */
	ASSERT_EQ(17, vimnav.x);  /* Closing quote */

	vimnav_exit();
	mock_term_free();
}

/* Test: vi( selects inner parentheses */
TEST(vimnav_vi_paren_selects_inner_parens)
{
	mock_term_init(24, 80);
	mock_set_line(5, "func(arg1, arg2) done");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 8;  /* Cursor on '1' inside parens */
	vimnav.y = 5;
	vimnav.savedx = 8;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then '(' for inner parens */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('(', 0);
	ASSERT_EQ(1, handled);

	/* Should select "arg1, arg2" (5-14), not including parens */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(5, vimnav.anchor_x);  /* After ( */
	ASSERT_EQ(14, vimnav.x);  /* Before ) */

	vimnav_exit();
	mock_term_free();
}

/* Test: vi) also selects inner parentheses (alias) */
TEST(vimnav_vi_close_paren_selects_inner_parens)
{
	mock_term_init(24, 80);
	mock_set_line(5, "func(arg1, arg2) done");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 8;
	vimnav.y = 5;
	vimnav.savedx = 8;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then ')' for inner parens */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key(')', 0);
	ASSERT_EQ(1, handled);

	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(5, vimnav.anchor_x);
	ASSERT_EQ(14, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: vi{ selects inner braces */
TEST(vimnav_vi_brace_selects_inner_braces)
{
	mock_term_init(24, 80);
	mock_set_line(5, "if {x == 1} then");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 5;  /* Cursor on 'x' inside braces */
	vimnav.y = 5;
	vimnav.savedx = 5;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then '{' for inner braces */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('{', 0);
	ASSERT_EQ(1, handled);

	/* Should select "x == 1" (4-9), not including braces */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(4, vimnav.anchor_x);
	ASSERT_EQ(9, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: vi[ selects inner brackets */
TEST(vimnav_vi_bracket_selects_inner_brackets)
{
	mock_term_init(24, 80);
	mock_set_line(5, "arr[0, 1, 2] end");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 6;  /* Cursor on '1' inside brackets */
	vimnav.y = 5;
	vimnav.savedx = 6;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then '[' for inner brackets */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('[', 0);
	ASSERT_EQ(1, handled);

	/* Should select "0, 1, 2" (4-10), not including brackets */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(4, vimnav.anchor_x);
	ASSERT_EQ(10, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: i/a on prompt line passes through to zsh */
TEST(vimnav_text_objects_prompt_passthrough)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% echo hello");

	term.c.x = 5;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 3;

	vimnav_enter();

	/* 'i' on prompt line should pass to zsh (insert mode) */
	int handled = vimnav_handle_key('i', 0);
	ASSERT_EQ(0, handled);  /* Pass through */

	/* 'a' on prompt line should pass to zsh (append mode) */
	handled = vimnav_handle_key('a', 0);
	ASSERT_EQ(0, handled);  /* Pass through */

	vimnav_exit();
	mock_term_free();
}

/* Test: nested parentheses handled correctly */
TEST(vimnav_vi_paren_nested)
{
	mock_term_init(24, 80);
	mock_set_line(5, "f(g(x), h(y))");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 4;  /* Cursor on 'x' inside inner parens */
	vimnav.y = 5;
	vimnav.savedx = 4;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then '(' for inner parens */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('(', 0);
	ASSERT_EQ(1, handled);

	/* Should select just "x" (4-4) from innermost parens g(x) */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(4, vimnav.anchor_x);
	ASSERT_EQ(4, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: vib is alias for vi( */
TEST(vimnav_vib_selects_inner_parens)
{
	mock_term_init(24, 80);
	mock_set_line(5, "func(arg) end");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 6;  /* Cursor on 'r' inside parens */
	vimnav.y = 5;
	vimnav.savedx = 6;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then 'b' for inner block (parens) */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('b', 0);
	ASSERT_EQ(1, handled);

	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(5, vimnav.anchor_x);  /* After ( */
	ASSERT_EQ(7, vimnav.x);  /* Before ) */

	vimnav_exit();
	mock_term_free();
}

/* Test: viB is alias for vi{ */
TEST(vimnav_viB_selects_inner_braces)
{
	mock_term_init(24, 80);
	mock_set_line(5, "if {test} end");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 5;  /* Cursor on 'e' inside braces */
	vimnav.y = 5;
	vimnav.savedx = 5;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then 'B' for inner Block (braces) */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('B', 0);
	ASSERT_EQ(1, handled);

	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(4, vimnav.anchor_x);  /* After { */
	ASSERT_EQ(7, vimnav.x);  /* Before } */

	vimnav_exit();
	mock_term_free();
}

/* Test: vi( searches right when cursor not inside parens */
TEST(vimnav_vi_paren_searches_right)
{
	mock_term_init(24, 80);
	mock_set_line(5, "This line is a test (test)");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 0;  /* Cursor at "T" of "This" - not inside parens */
	vimnav.y = 5;
	vimnav.savedx = 0;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then '(' */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('(', 0);
	ASSERT_EQ(1, handled);

	/* Should select "test" inside the parens (21-24) */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(21, vimnav.anchor_x);  /* After ( */
	ASSERT_EQ(24, vimnav.x);  /* Before ) */

	vimnav_exit();
	mock_term_free();
}

/* Test: vi( does nothing when no enclosing space exists */
TEST(vimnav_vi_paren_no_closing)
{
	mock_term_init(24, 80);
	mock_set_line(5, "This line is also ( a test");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 5;  /* Cursor somewhere on the line */
	vimnav.y = 5;
	vimnav.savedx = 5;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then '(' */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('(', 0);

	/* Should not find anything - no valid pair, but key consumed in visual mode */
	ASSERT_EQ(1, handled);  /* Consumed to prevent leaking to zsh */
	ASSERT_EQ(2, vimnav.mode);  /* Still in VIMNAV_VISUAL (unchanged) */

	vimnav_exit();
	mock_term_free();
}

/* Test: vi( does nothing when parens are to the left of cursor */
TEST(vimnav_vi_paren_pair_to_left)
{
	mock_term_init(24, 80);
	mock_set_line(5, "This line (is a) super test");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 23;  /* Cursor on "test" - parens are to the left */
	vimnav.y = 5;
	vimnav.savedx = 23;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then '(' */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('(', 0);

	/* Should not find anything - pair is to the left, but key consumed in visual mode */
	ASSERT_EQ(1, handled);  /* Consumed to prevent leaking to zsh */
	ASSERT_EQ(2, vimnav.mode);  /* Still in VIMNAV_VISUAL (unchanged) */

	vimnav_exit();
	mock_term_free();
}

/* Test: vi" searches right when cursor not inside quotes */
TEST(vimnav_vi_quote_searches_right)
{
	mock_term_init(24, 80);
	mock_set_line(5, "echo hello \"world\" done");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 0;  /* Cursor at start - not inside quotes */
	vimnav.y = 5;
	vimnav.savedx = 0;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then '"' */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('"', 0);
	ASSERT_EQ(1, handled);

	/* Should select "world" inside the quotes (12-16) */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(12, vimnav.anchor_x);  /* After opening " */
	ASSERT_EQ(16, vimnav.x);  /* Before closing " */

	vimnav_exit();
	mock_term_free();
}

/* Test: vi[ searches right when cursor not inside brackets */
TEST(vimnav_vi_bracket_searches_right)
{
	mock_term_init(24, 80);
	mock_set_line(5, "array access [0] here");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 0;  /* Cursor at start */
	vimnav.y = 5;
	vimnav.savedx = 0;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then '[' */
	vimnav_handle_key('i', 0);
	int handled = vimnav_handle_key('[', 0);
	ASSERT_EQ(1, handled);

	/* Should select "0" inside the brackets */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(14, vimnav.anchor_x);  /* After [ */
	ASSERT_EQ(14, vimnav.x);  /* Before ] */

	vimnav_exit();
	mock_term_free();
}

/* Test: va( searches right and includes parens */
TEST(vimnav_va_paren_searches_right)
{
	mock_term_init(24, 80);
	mock_set_line(5, "call func(arg) here");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 0;  /* Cursor at start */
	vimnav.y = 5;
	vimnav.savedx = 0;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'a' then '(' */
	vimnav_handle_key('a', 0);
	int handled = vimnav_handle_key('(', 0);
	ASSERT_EQ(1, handled);

	/* Should select "(arg)" including parens */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */
	ASSERT_EQ(9, vimnav.anchor_x);  /* Opening ( */
	ASSERT_EQ(13, vimnav.x);  /* Closing ) */

	vimnav_exit();
	mock_term_free();
}

/* Test: i/a in normal mode from history snap to prompt and pass to zsh */
TEST(vimnav_ia_normal_mode_snaps_to_prompt)
{
	mock_term_init(24, 80);
	mock_set_line(5, "history line");
	mock_set_line(10, "% prompt");

	/* Setup in history (scrolled up) */
	term.c.x = 5;
	term.c.y = 10;
	term.scr = 5;  /* Scrolled up into history */
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 5;  /* Viewing history line */
	vimnav.x = 3;

	/* 'i' in normal mode should snap to prompt and pass to zsh */
	int handled = vimnav_handle_key('i', 0);
	ASSERT_EQ(0, handled);  /* Passed to zsh */
	ASSERT_EQ(0, term.scr);  /* Scrolled back to bottom */
	ASSERT_EQ(10, vimnav.y);  /* At prompt line */

	/* Reset state */
	term.scr = 5;
	vimnav.y = 5;
	vimnav.x = 3;

	/* 'a' in normal mode should also snap to prompt and pass to zsh */
	handled = vimnav_handle_key('a', 0);
	ASSERT_EQ(0, handled);  /* Passed to zsh */
	ASSERT_EQ(0, term.scr);  /* Scrolled back to bottom */
	ASSERT_EQ(10, vimnav.y);  /* At prompt line */

	vimnav_exit();
	mock_term_free();
}

/* Test: pending text object cleared on unknown key */
TEST(vimnav_textobj_unknown_key_clears_pending)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 3;
	vimnav.y = 5;
	vimnav.savedx = 3;

	/* Press 'v' to enter visual mode first */
	vimnav_handle_key('v', 0);

	/* Press 'i' then an invalid key 'z' */
	vimnav_handle_key('i', 0);
	ASSERT_EQ('i', vimnav.pending_textobj);

	int handled = vimnav_handle_key('z', 0);
	/* Should clear pending; key consumed in visual mode to prevent leaking to zsh */
	ASSERT_EQ(0, vimnav.pending_textobj);
	ASSERT_EQ(1, handled);  /* Consumed in visual mode */

	vimnav_exit();
	mock_term_free();
}

/* Test: scrolling up works after Ctrl+L clears screen (but history has content)
 * This is a regression test for the bug where scrolling was blocked after clear
 * because the check only looked at the immediate top line (which was empty). */
TEST(vimnav_scroll_up_after_clear)
{
	mock_term_init(24, 80);

	/* Simulate post-Ctrl+L state:
	 * - Screen is cleared (all lines empty)
	 * - History has content from before the clear
	 * - term.scr = 0 (not scrolled)
	 */

	/* All screen lines are empty (default from mock_term_init) */

	/* Set up history with content - simulate what Ctrl+L would have saved */
	term.histi = 3;  /* History has 3 lines */
	mock_set_hist(1, "output line 1");
	mock_set_hist(2, "output line 2");
	mock_set_hist(3, "output line 3");

	/* Cursor at bottom of screen (prompt position after clear) */
	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.y = 0;  /* At top of visible screen */
	vimnav.x = 0;
	vimnav.savedx = 0;

	/* Try to scroll up with 'k' - should work because history has content */
	int handled = vimnav_handle_key('k', 0);
	ASSERT_EQ(1, handled);

	/* term.scr should have increased (scrolled into history) */
	ASSERT(term.scr > 0);

	vimnav_exit();
	mock_term_free();
}

/* Test: Ctrl+u scrolls up after screen clear */
TEST(vimnav_ctrl_u_after_clear)
{
	mock_term_init(24, 80);

	/* All screen lines empty, history has content */
	term.histi = 5;
	mock_set_hist(1, "history 1");
	mock_set_hist(2, "history 2");
	mock_set_hist(3, "history 3");
	mock_set_hist(4, "history 4");
	mock_set_hist(5, "history 5");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.y = 0;
	vimnav.x = 0;
	vimnav.savedx = 0;

	/* Ctrl+u should scroll up */
	int handled = vimnav_handle_key('u', 4);  /* 4 = ControlMask */
	ASSERT_EQ(1, handled);

	/* Should have scrolled */
	ASSERT(term.scr > 0);

	vimnav_exit();
	mock_term_free();
}

/* Test: scrolling stops when history is truly empty */
TEST(vimnav_scroll_stops_at_empty_history)
{
	mock_term_init(24, 80);

	/* Screen empty, history also empty (no content ever saved) */
	term.histi = 0;
	/* All history lines are blank (default from init) */

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.y = 0;
	vimnav.x = 0;

	/* Try to scroll up - should NOT work because history is empty */
	vimnav_handle_key('k', 0);

	/* term.scr should still be 0 (no scrolling happened) */
	ASSERT_EQ(0, term.scr);

	vimnav_exit();
	mock_term_free();
}

/* Test: can scroll to see ALL history lines including the oldest
 * Regression test: history index calculation must match TLINE macro (+1) */
TEST(vimnav_scroll_reaches_oldest_history)
{
	mock_term_init(24, 80);

	/* Simulate: user ran "ls" which output 2 lines, then Ctrl+L
	 * History should have (in order of saving):
	 * hist[1] = "% ls" (original command)
	 * hist[2] = "file1"
	 * hist[3] = "file2"
	 */
	term.histi = 3;
	mock_set_hist(1, "% ls");
	mock_set_hist(2, "file1");
	mock_set_hist(3, "file2");

	/* Screen is cleared (empty) */
	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.y = 0;
	vimnav.x = 0;
	vimnav.savedx = 0;

	/* Scroll up 3 times - should be able to reach all 3 history lines */
	vimnav_handle_key('k', 0);  /* scr = 1, see file2 */
	ASSERT_EQ(1, term.scr);

	vimnav_handle_key('k', 0);  /* scr = 2, see file1 */
	ASSERT_EQ(2, term.scr);

	vimnav_handle_key('k', 0);  /* scr = 3, see "% ls" - THIS WAS THE BUG */
	ASSERT_EQ(3, term.scr);

	/* Fourth scroll should NOT work - no more history */
	vimnav_handle_key('k', 0);
	ASSERT_EQ(3, term.scr);  /* Still 3, didn't scroll further */

	vimnav_exit();
	mock_term_free();
}

/* Test: f finds character forward on current line */
TEST(vimnav_f_finds_char_forward)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world test");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 0;  /* Start at 'h' */
	vimnav.y = 5;
	vimnav.savedx = 0;

	/* Press 'f' then 'o' to find first 'o' */
	int handled = vimnav_handle_key('f', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ('f', vimnav.pending_find);

	handled = vimnav_handle_key('o', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, vimnav.pending_find);  /* Cleared */
	ASSERT_EQ(4, vimnav.x);  /* 'o' in "hello" at index 4 */

	/* State should be saved for repeat */
	ASSERT_EQ('o', vimnav.last_find_char);
	ASSERT_EQ(1, vimnav.last_find_forward);

	vimnav_exit();
	mock_term_free();
}

/* Test: F finds character backward on current line */
TEST(vimnav_F_finds_char_backward)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world test");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 15;  /* Start at 't' of "test" */
	vimnav.y = 5;
	vimnav.savedx = 15;

	/* Press 'F' then 'o' to find 'o' backward */
	int handled = vimnav_handle_key('F', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ('F', vimnav.pending_find);

	handled = vimnav_handle_key('o', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, vimnav.pending_find);  /* Cleared */
	ASSERT_EQ(7, vimnav.x);  /* 'o' in "world" at index 7 */

	/* State should be saved for repeat */
	ASSERT_EQ('o', vimnav.last_find_char);
	ASSERT_EQ(0, vimnav.last_find_forward);  /* Backward */

	vimnav_exit();
	mock_term_free();
}

/* Test: f stays put when char not found */
TEST(vimnav_f_no_match_stays_put)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 3;
	vimnav.y = 5;
	vimnav.savedx = 3;

	/* Press 'f' then 'z' - not in line */
	vimnav_handle_key('f', 0);
	vimnav_handle_key('z', 0);

	ASSERT_EQ(3, vimnav.x);  /* Didn't move */

	vimnav_exit();
	mock_term_free();
}

/* Test: ; repeats f search forward */
TEST(vimnav_semicolon_repeats_f_forward)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world woot");
	/* Indices:      01234567890123456
	 * 'o' at: 4, 7, 13, 14 */

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 0;
	vimnav.y = 5;
	vimnav.savedx = 0;

	/* Find first 'o' with fo */
	vimnav_handle_key('f', 0);
	vimnav_handle_key('o', 0);
	ASSERT_EQ(4, vimnav.x);  /* First 'o' at index 4 */

	/* Press ; to find next 'o' */
	int handled = vimnav_handle_key(';', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(7, vimnav.x);  /* Second 'o' in "world" at index 7 */

	/* Press ; again */
	handled = vimnav_handle_key(';', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(13, vimnav.x);  /* Third 'o' in "woot" at index 13 */

	vimnav_exit();
	mock_term_free();
}

/* Test: , repeats f search in reverse (backward) */
TEST(vimnav_comma_repeats_f_backward)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world woot");
	/* Indices:      01234567890123456
	 * 'o' at: 4, 7, 13, 14 */

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 0;
	vimnav.y = 5;
	vimnav.savedx = 0;

	/* Find 'o' multiple times to get to third one */
	vimnav_handle_key('f', 0);
	vimnav_handle_key('o', 0);
	vimnav_handle_key(';', 0);
	vimnav_handle_key(';', 0);
	ASSERT_EQ(13, vimnav.x);  /* At third 'o' */

	/* Press , to go back (opposite direction) */
	int handled = vimnav_handle_key(',', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(7, vimnav.x);  /* Back to second 'o' */

	/* Press , again */
	handled = vimnav_handle_key(',', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(4, vimnav.x);  /* Back to first 'o' */

	vimnav_exit();
	mock_term_free();
}

/* Test: ; repeats F search backward */
TEST(vimnav_semicolon_repeats_F_backward)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world woot");
	/* Indices:      01234567890123456
	 * 'o' at: 4, 7, 13, 14 */

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 15;  /* Start at 't' of "woot" */
	vimnav.y = 5;
	vimnav.savedx = 15;

	/* Find 'o' backward with Fo */
	vimnav_handle_key('F', 0);
	vimnav_handle_key('o', 0);
	ASSERT_EQ(14, vimnav.x);  /* Second 'o' in "woot" at index 14 */

	/* Press ; to continue backward */
	int handled = vimnav_handle_key(';', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(13, vimnav.x);  /* First 'o' in "woot" at index 13 */

	/* Press ; again */
	handled = vimnav_handle_key(';', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(7, vimnav.x);  /* 'o' in "world" */

	vimnav_exit();
	mock_term_free();
}

/* Test: , reverses F search (goes forward) */
TEST(vimnav_comma_repeats_F_forward)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world woot");
	/* Indices:      01234567890123456
	 * 'o' at: 4, 7, 13, 14 */

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.x = 15;
	vimnav.y = 5;
	vimnav.savedx = 15;

	/* Find 'o' backward with Fo, then ; twice */
	vimnav_handle_key('F', 0);
	vimnav_handle_key('o', 0);
	vimnav_handle_key(';', 0);
	vimnav_handle_key(';', 0);
	ASSERT_EQ(7, vimnav.x);  /* At 'o' in "world" */

	/* Press , to go forward (opposite of F's direction) */
	int handled = vimnav_handle_key(',', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(13, vimnav.x);  /* Forward to first 'o' in "woot" */

	vimnav_exit();
	mock_term_free();
}

/* Test: f/F/;/, pass through on prompt line */
TEST(vimnav_fF_prompt_passthrough)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% echo hello");

	term.c.x = 5;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 3;

	vimnav_enter();

	/* f on prompt line should pass to zsh */
	int handled = vimnav_handle_key('f', 0);
	ASSERT_EQ(0, handled);

	/* F on prompt line should pass to zsh */
	handled = vimnav_handle_key('F', 0);
	ASSERT_EQ(0, handled);

	/* ; on prompt line should pass to zsh */
	handled = vimnav_handle_key(';', 0);
	ASSERT_EQ(0, handled);

	/* , on prompt line should pass to zsh */
	handled = vimnav_handle_key(',', 0);
	ASSERT_EQ(0, handled);

	vimnav_exit();
	mock_term_free();
}

/* Test: H moves cursor to top of visible screen */
TEST(vimnav_H_moves_to_screen_top)
{
	mock_term_init(24, 80);
	mock_set_line(0, "top line");
	mock_set_line(10, "middle line");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 10;  /* Start in middle of screen */
	vimnav.x = 5;
	vimnav.savedx = 5;

	int handled = vimnav_handle_key('H', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, vimnav.y);  /* Moved to top (row 0) */

	vimnav_exit();
	mock_term_free();
}

/* Test: L moves cursor to bottom visible line (prompt line when not scrolled) */
TEST(vimnav_L_moves_to_screen_bottom)
{
	mock_term_init(24, 80);
	mock_set_line(5, "history line");
	mock_set_line(20, "% prompt text");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 3;

	vimnav_enter();
	vimnav.y = 5;  /* Start in history */
	vimnav.x = 3;
	vimnav.savedx = 3;

	int handled = vimnav_handle_key('L', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(20, vimnav.y);  /* Bottom visible line = prompt when not scrolled */
	ASSERT(tisvimnav());

	vimnav_exit();
	mock_term_free();
}

/* Test: L moves to bottom of visible screen when scrolled up (not to prompt) */
TEST(vimnav_L_stays_on_screen_when_scrolled)
{
	mock_term_init(24, 80);
	mock_set_line(5, "history line");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	/* Simulate user scrolling up after entering nav mode */
	term.scr = 5;
	vimnav.y = 5;
	vimnav.x = 3;
	vimnav.savedx = 3;

	int handled = vimnav_handle_key('L', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(5, term.scr);  /* Should NOT scroll down */
	ASSERT_EQ(23, vimnav.y);  /* Bottom of visible screen (row - 1) */
	ASSERT(tisvimnav());

	vimnav_exit();
	mock_term_free();
}

/* Test: M moves cursor to middle between top and prompt */
TEST(vimnav_M_moves_to_middle)
{
	mock_term_init(24, 80);
	mock_set_line(0, "top line");
	mock_set_line(10, "middle line");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;  /* Prompt at row 20 */
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 0;  /* Start at top */
	vimnav.x = 0;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('M', 0);
	ASSERT_EQ(1, handled);
	/* Middle = 20 / 2 = 10 */
	ASSERT_EQ(10, vimnav.y);

	vimnav_exit();
	mock_term_free();
}

/* Test: M uses screen bottom when scrolled */
TEST(vimnav_M_middle_when_scrolled)
{
	mock_term_init(24, 80);
	for (int i = 0; i < 24; i++) {
		mock_set_line(i, "line content");
	}

	term.c.x = 5;
	term.c.y = 23;  /* Prompt at bottom */
	term.scr = 10;  /* Scrolled up - prompt not visible */
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 0;
	vimnav.x = 0;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('M', 0);
	ASSERT_EQ(1, handled);
	/* When scrolled, bottom = term.row - 1 = 23, middle = 23 / 2 = 11 */
	ASSERT_EQ(11, vimnav.y);

	vimnav_exit();
	mock_term_free();
}

/* Test: L goes to prompt row (not term.row-1) when scrolled but prompt visible */
TEST(vimnav_L_scrolled_prompt_visible)
{
	mock_term_init(24, 80);
	mock_set_line(0, "% prompt");

	term.c.x = 5;
	term.c.y = 0;
	vimnav.zsh_cursor = 3;

	vimnav_enter();
	/* Simulate scrolling up after Ctrl+L */
	term.scr = 5;
	vimnav.y = 2;
	vimnav.x = 0;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('L', 0);
	ASSERT_EQ(1, handled);
	/* Prompt screen row = 0+5 = 5, not 23 */
	ASSERT_EQ(5, vimnav.y);
	/* Should sync x to zsh cursor since we landed on prompt */
	ASSERT_EQ(5, vimnav.x);  /* prompt_end (2) + zsh_cursor (3) */

	vimnav_exit();
	mock_term_free();
}

/* Test: M computes middle from prompt screen row when scrolled but visible */
TEST(vimnav_M_scrolled_prompt_visible)
{
	mock_term_init(24, 80);
	mock_set_line(0, "% prompt");

	term.c.x = 5;
	term.c.y = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	term.scr = 10;
	vimnav.y = 5;
	vimnav.x = 0;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('M', 0);
	ASSERT_EQ(1, handled);
	/* Prompt screen row = 0+10 = 10, middle = 10/2 = 5 */
	ASSERT_EQ(5, vimnav.y);

	vimnav_exit();
	mock_term_free();
}

/* Test: gg (double g) moves to top of history */
TEST(vimnav_gg_moves_to_top)
{
	mock_term_init(24, 80);
	mock_set_line(0, "oldest line");
	mock_set_line(5, "middle line");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 10;  /* Start somewhere in the middle */
	vimnav.x = 3;
	vimnav.savedx = 3;

	/* First g sets pending, doesn't move */
	int handled = vimnav_handle_key('g', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(1, vimnav.pending_g);
	ASSERT_EQ(10, vimnav.y);  /* Didn't move yet */

	/* Second g moves to top */
	handled = vimnav_handle_key('g', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, vimnav.pending_g);  /* Cleared */
	ASSERT_EQ(0, vimnav.y);  /* Moved to top */

	vimnav_exit();
	mock_term_free();
}

/* Test: single g followed by non-g key doesn't move */
TEST(vimnav_g_nonmatch_clears_pending)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 5;  /* Scrolled */
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 5;
	vimnav.x = 3;
	vimnav.savedx = 3;

	/* First g sets pending */
	vimnav_handle_key('g', 0);
	ASSERT_EQ(1, vimnav.pending_g);

	/* Non-g key clears pending and processes normally (j moves down) */
	int handled = vimnav_handle_key('j', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, vimnav.pending_g);  /* Cleared */
	ASSERT_EQ(6, vimnav.y);  /* Moved down, not to top */

	vimnav_exit();
	mock_term_free();
}

/* Test: G moves to bottom (prompt) and syncs cursor */
TEST(vimnav_G_moves_to_prompt)
{
	mock_term_init(24, 80);
	mock_set_line(0, "top line");
	mock_set_line(20, "% prompt text");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 10;  /* Scrolled up */
	vimnav.zsh_cursor = 3;  /* zsh cursor at offset 3 */

	vimnav_enter();
	vimnav.y = 0;  /* Start at top */
	vimnav.x = 0;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('G', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, term.scr);  /* Scrolled back to bottom */
	ASSERT_EQ(20, vimnav.y);  /* At prompt line */
	/* Cursor synced: prompt_end (2) + zsh_cursor (3) = 5 */
	ASSERT_EQ(5, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: gg inherits visual mode from zsh when leaving prompt */
TEST(vimnav_gg_inherits_zsh_visual)
{
	mock_term_init(24, 80);
	mock_set_line(0, "history line");
	mock_set_line(20, "% command text");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;  /* At prompt */
	vimnav.zsh_cursor = 5;
	vimnav.zsh_visual = 0;  /* Not in visual mode at entry */

	vimnav_enter();
	ASSERT_EQ(1, vimnav.mode);  /* VIMNAV_NORMAL */

	/* Now simulate zsh entering visual mode while we're in nav mode at prompt */
	vimnav.zsh_visual = 1;
	vimnav.zsh_visual_anchor = 2;
	vimnav.zsh_visual_line = 0;

	/* gg should inherit selection when leaving prompt space */
	vimnav_handle_key('g', 0);
	vimnav_handle_key('g', 0);

	ASSERT_EQ(0, vimnav.y);  /* At top */
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL - inherited zsh visual mode */

	vimnav_exit();
	mock_term_free();
}

/* Test vimnav_curline_y returns -1 when not in nav mode */
TEST(vimnav_curline_y_inactive)
{
	mock_term_init(80, 24);
	term.c.y = 23;

	/* Not in nav mode */
	vimnav.mode = 0;
	ASSERT_EQ(-1, vimnav_curline_y());

	mock_term_free();
}

/* Test vimnav_curline_y returns -1 when in prompt space */
TEST(vimnav_curline_y_prompt_space)
{
	mock_term_init(80, 24);
	term.c.y = 23;
	term.scr = 0;
	mock_set_line(23, "% cmd");

	vimnav_enter();
	/* Cursor is at prompt line (term.c.y), which is prompt space */
	ASSERT_EQ(23, vimnav.y);
	ASSERT_EQ(-1, vimnav_curline_y());  /* Should not highlight in prompt space */

	vimnav_exit();
	mock_term_free();
}

/* Test vimnav_curline_y returns y when scrolled into history */
TEST(vimnav_curline_y_history)
{
	int i;
	mock_term_init(80, 24);

	/* Add some history */
	for (i = 0; i < 30; i++) {
		mock_set_hist(i, "history line");
	}
	term.c.y = 23;
	term.scr = 0;
	mock_set_line(23, "% cmd");

	vimnav_enter();

	/* Scroll up into history */
	vimnav_handle_key('k', 0);  /* Move up to row 22 */
	ASSERT_EQ(22, vimnav.y);
	/* Row 22 is still "prompt space" since scr==0 and prompt_start would be <=22
	 * Let's scroll up more to get into real history */

	term.scr = 5;  /* Simulate scrolled into history */
	vimnav.y = 10;  /* Cursor at row 10 on screen */

	ASSERT_EQ(10, vimnav_curline_y());  /* Should return y when in history */

	vimnav_exit();
	mock_term_free();
}

/* Test vimnav_curline_y returns -1 when in visual mode */
TEST(vimnav_curline_y_visual_mode)
{
	int i;
	mock_term_init(80, 24);

	/* Add some history */
	for (i = 0; i < 30; i++) {
		mock_set_hist(i, "history line");
	}
	term.c.y = 23;
	term.scr = 5;  /* Scrolled into history */
	mock_set_line(23, "% cmd");

	vimnav_enter();
	vimnav.y = 10;  /* Cursor in history area */

	/* In normal mode, should highlight */
	ASSERT_EQ(1, vimnav.mode);  /* VIMNAV_NORMAL */
	ASSERT_EQ(10, vimnav_curline_y());

	/* Enter visual mode with 'v' - should stop highlighting */
	vimnav.mode = 2;  /* VIMNAV_VISUAL */
	ASSERT_EQ(-1, vimnav_curline_y());

	/* Visual line mode - should also stop highlighting */
	vimnav.mode = 3;  /* VIMNAV_VISUAL_LINE */
	ASSERT_EQ(-1, vimnav_curline_y());

	/* Back to normal mode - should highlight again */
	vimnav.mode = 1;  /* VIMNAV_NORMAL */
	ASSERT_EQ(10, vimnav_curline_y());

	vimnav_exit();
	mock_term_free();
}

/* Test vimnav_curline_y returns -1 when vimnav.y is below cursor (empty space after clear) */
TEST(vimnav_curline_y_below_cursor)
{
	mock_term_init(24, 80);
	mock_set_line(0, "% prompt");

	/* Simulate state after Ctrl+L clears screen:
	 * - term.scr is 0 (not scrolled)
	 * - term.c.y is 0 (cursor at top after clear)
	 * - vimnav.y is still at old position (e.g., 10) */
	term.c.y = 0;
	term.scr = 0;

	vimnav_enter();
	vimnav.y = 10;  /* Stale position pointing to empty space */
	vimnav.mode = 1;  /* VIMNAV_NORMAL */
	vimnav.zsh_visual = 0;

	/* Should return -1 since vimnav.y (10) > prompt screen row (0+0=0) */
	ASSERT_EQ(-1, vimnav_curline_y());

	/* Even when scrolled, highlight shouldn't show below prompt screen row.
	 * Prompt is at screen row 0+5=5, vimnav.y=10 is past it (empty space). */
	term.scr = 5;
	ASSERT_EQ(-1, vimnav_curline_y());

	/* But highlight should work at a valid history row above the prompt */
	vimnav.y = 3;  /* History row, above prompt at screen row 5 */
	ASSERT_EQ(3, vimnav_curline_y());

	vimnav_exit();
	mock_term_free();
}

/* Test: empty lines are selectable in visual mode (column 0 acts as virtual newline) */
TEST(vimnav_visual_empty_line_selectable)
{
	mock_term_init(24, 80);
	mock_set_line(5, "content line");
	/* Line 6 is empty (not set, so tlinelen returns 0) */
	mock_set_line(7, "another line");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 3;  /* Scrolled into history */

	vimnav_enter();
	vimnav.x = 0;
	vimnav.y = 5;  /* Start on content line */
	vimnav.savedx = 0;

	/* Enter visual line mode */
	vimnav_handle_key('V', 0);
	ASSERT_EQ(3, vimnav.mode);  /* VIMNAV_VISUAL_LINE */

	/* Move down to empty line */
	vimnav_handle_key('j', 0);
	ASSERT_EQ(6, vimnav.y);  /* Now on empty line */

	/* Column 0 of the empty line should be selected (for visual feedback) */
	ASSERT_EQ(1, selected(0, 6));

	vimnav_exit();
	mock_term_free();
}

/* Test: editing keys (d, x, c, etc.) clear visual mode before snapping to prompt.
 * Regression: pressing 'd' in visual line mode left ghost selection and didn't sync with zsh. */
TEST(vimnav_editing_keys_clear_visual_on_snap)
{
	char editing_keys[] = { 'x', 'X', 'd', 'D', 'c', 'C', 's', 'S', 'r', 'R',
	                        'A', 'I', 'o', 'O', 'u', '.', '~', 0 };

	for (int i = 0; editing_keys[i]; i++) {
		mock_term_init(24, 80);
		mock_set_line(5, "some history line");

		term.c.x = 0;
		term.c.y = 23;
		term.scr = 0;

		vimnav_enter();
		vimnav.y = 5;
		vimnav.x = 3;
		vimnav.savedx = 3;

		/* Enter visual line mode */
		vimnav_handle_key('V', 0);
		ASSERT_EQ(3, vimnav.mode);  /* VIMNAV_VISUAL_LINE */

		/* Press editing key - should clear visual and snap to prompt */
		vimnav_handle_key(editing_keys[i], 0);
		ASSERT_EQ(1, vimnav.mode);  /* VIMNAV_NORMAL (visual cleared) */
		ASSERT_EQ(23, vimnav.y);    /* Snapped to prompt */

		vimnav_exit();
		mock_term_free();
	}
}

/* Test: unrecognized keys are consumed in visual mode (not leaked to zsh) */
TEST(vimnav_visual_mode_consumes_unknown_keys)
{
	mock_term_init(24, 80);
	mock_set_line(5, "hello world");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.y = 5;
	vimnav.x = 3;
	vimnav.savedx = 3;

	/* Enter visual mode */
	vimnav_handle_key('v', 0);
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */

	/* Press unrecognized keys - should be consumed, not passed to zsh */
	ASSERT_EQ(1, vimnav_handle_key('z', 0));
	ASSERT_EQ(1, vimnav_handle_key('q', 0));
	ASSERT_EQ(1, vimnav_handle_key('n', 0));
	ASSERT_EQ(2, vimnav.mode);  /* Still in VIMNAV_VISUAL */

	/* In normal mode, unrecognized keys should NOT be consumed */
	vimnav_handle_key('v', 0);  /* Exit visual */
	ASSERT_EQ(1, vimnav.mode);  /* VIMNAV_NORMAL */
	ASSERT_EQ(0, vimnav_handle_key('z', 0));  /* Not consumed */

	vimnav_exit();
	mock_term_free();
}

/* Test: force_enter works even on alt screen */
TEST(vimnav_force_enter_works_on_altscreen)
{
	mock_term_init(24, 80);
	mock_set_line(10, "TUI content here");
	term.mode = MODE_ALTSCREEN;
	term.c.x = 5;
	term.c.y = 10;

	/* Normal enter should fail on alt screen */
	vimnav_enter();
	ASSERT(!tisvimnav());

	/* Force enter should work */
	vimnav_force_enter();
	ASSERT(tisvimnav());
	ASSERT_EQ(1, vimnav.forced);
	ASSERT_EQ(5, vimnav.x);
	ASSERT_EQ(10, vimnav.y);

	vimnav_exit();
	ASSERT(!tisvimnav());
	ASSERT_EQ(0, vimnav.forced);

	mock_term_free();
}

/* Test: force_enter doesn't double-enter if already in nav mode */
TEST(vimnav_force_enter_no_double_entry)
{
	mock_term_init(24, 80);
	term.c.x = 0;
	term.c.y = 10;

	vimnav_force_enter();
	ASSERT(tisvimnav());

	/* Second force_enter should be a no-op */
	vimnav.x = 5;  /* Change position to detect if it resets */
	vimnav_force_enter();
	ASSERT_EQ(5, vimnav.x);  /* Shouldn't have been reset */

	vimnav_exit();
	mock_term_free();
}

/* Test: setting forced flag on regular nav mode upgrades to forced behavior */
TEST(vimnav_upgrade_regular_to_forced)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% echo hello");
	term.c.x = 5;
	term.c.y = 10;
	term.scr = 0;
	vimnav.zsh_cursor = 3;

	/* Enter regular nav mode */
	vimnav_enter();
	ASSERT(tisvimnav());
	ASSERT_EQ(0, vimnav.forced);

	/* In regular nav mode, h on prompt passes to zsh */
	vimnav.x = 5;
	vimnav.savedx = 5;
	ASSERT_EQ(0, vimnav_handle_key('h', 0));  /* Passed to zsh */

	/* Upgrade to forced mode (simulates what kpress does on Shift+Esc) */
	vimnav.forced = 1;

	/* Now h on same line should be handled by st */
	vimnav.x = 5;
	vimnav.savedx = 5;
	ASSERT_EQ(1, vimnav_handle_key('h', 0));  /* Handled by st */
	ASSERT_EQ(4, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: Escape does NOT exit forced nav mode */
TEST(vimnav_forced_escape_stays)
{
	mock_term_init(24, 80);
	mock_set_line(10, "some content");
	term.c.x = 3;
	term.c.y = 10;

	vimnav_force_enter();
	ASSERT(tisvimnav());

	int handled = vimnav_handle_key(XK_Escape, 0);
	ASSERT_EQ(1, handled);
	ASSERT(tisvimnav());
	ASSERT_EQ(1, vimnav.mode);  /* Still VIMNAV_NORMAL */

	mock_term_free();
}

/* Test: Escape in forced visual mode clears visual but stays in nav mode */
TEST(vimnav_forced_escape_clears_visual_first)
{
	mock_term_init(24, 80);
	mock_set_line(10, "some content");
	term.c.x = 3;
	term.c.y = 10;

	vimnav_force_enter();
	vimnav_handle_key('v', 0);
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */

	/* First Escape clears visual, stays in normal */
	int handled = vimnav_handle_key(XK_Escape, 0);
	ASSERT_EQ(1, handled);
	ASSERT(tisvimnav());
	ASSERT_EQ(1, vimnav.mode);  /* VIMNAV_NORMAL */

	/* Second Escape stays in forced nav mode (does not exit) */
	handled = vimnav_handle_key(XK_Escape, 0);
	ASSERT_EQ(1, handled);
	ASSERT(tisvimnav());
	ASSERT_EQ(1, vimnav.mode);  /* Still VIMNAV_NORMAL */

	mock_term_free();
}

/* Test: i exits forced nav mode */
TEST(vimnav_forced_i_exits)
{
	mock_term_init(24, 80);
	mock_set_line(10, "some content");
	term.c.x = 3;
	term.c.y = 10;

	vimnav_force_enter();
	ASSERT(tisvimnav());

	int handled = vimnav_handle_key('i', 0);
	ASSERT_EQ(1, handled);
	ASSERT(!tisvimnav());

	mock_term_free();
}

/* Test: a exits forced nav mode */
TEST(vimnav_forced_a_exits)
{
	mock_term_init(24, 80);
	mock_set_line(10, "some content");
	term.c.x = 3;
	term.c.y = 10;

	vimnav_force_enter();
	ASSERT(tisvimnav());

	int handled = vimnav_handle_key('a', 0);
	ASSERT_EQ(1, handled);
	ASSERT(!tisvimnav());

	mock_term_free();
}

/* Test: editing keys are no-ops in forced mode */
TEST(vimnav_forced_editing_keys_noop)
{
	char editing_keys[] = { 'x', 'X', 'd', 'D', 'c', 'C', 's', 'S', 'r', 'R',
	                        'A', 'I', 'o', 'O', 'u', '.', '~', 'p', 0 };

	for (int i = 0; editing_keys[i]; i++) {
		mock_term_init(24, 80);
		mock_set_line(10, "some content");
		term.c.x = 3;
		term.c.y = 10;

		vimnav_force_enter();
		mock_reset();

		int handled = vimnav_handle_key(editing_keys[i], 0);
		ASSERT_EQ(1, handled);  /* Key consumed */
		ASSERT(tisvimnav());    /* Still in nav mode */
		ASSERT_EQ(0, mock_state.ttywrite_calls);  /* Nothing sent to shell */

		vimnav_exit();
		mock_term_free();
	}
}

/* Test: navigation keys work in forced mode */
TEST(vimnav_forced_navigation_works)
{
	mock_term_init(24, 80);
	mock_set_line(10, "hello world test");
	term.c.x = 3;
	term.c.y = 10;

	vimnav_force_enter();
	ASSERT_EQ(3, vimnav.x);
	ASSERT_EQ(10, vimnav.y);

	/* h moves left */
	vimnav_handle_key('h', 0);
	ASSERT_EQ(2, vimnav.x);

	/* l moves right */
	vimnav_handle_key('l', 0);
	ASSERT_EQ(3, vimnav.x);

	/* 0 moves to beginning */
	vimnav_handle_key('0', 0);
	ASSERT_EQ(0, vimnav.x);

	/* $ moves to end */
	vimnav_handle_key('$', 0);
	ASSERT_EQ(15, vimnav.x);  /* "hello world test" = 16 chars, last index 15 */

	vimnav_exit();
	mock_term_free();
}

/* Test: j moves below TUI cursor position in forced mode on altscreen */
TEST(vimnav_forced_j_below_cursor_altscreen)
{
	mock_term_init(24, 80);
	for (int i = 0; i < 24; i++)
		mock_set_line(i, "content line");
	term.c.x = 0;
	term.c.y = 10;
	term.mode |= MODE_ALTSCREEN;

	vimnav_force_enter();
	ASSERT_EQ(10, vimnav.y);

	/* j should move below the TUI cursor position */
	for (int i = 11; i <= 23; i++) {
		vimnav_handle_key('j', 0);
		ASSERT_EQ(i, vimnav.y);
	}

	/* At bottom, j is a no-op */
	vimnav_handle_key('j', 0);
	ASSERT_EQ(23, vimnav.y);

	vimnav_exit();
	mock_term_free();
}

/* Test: k at row 0 on altscreen does not scroll into main screen history */
TEST(vimnav_forced_k_no_history_scroll_altscreen)
{
	mock_term_init(24, 80);
	for (int i = 0; i < 24; i++)
		mock_set_line(i, "content line");
	term.c.x = 0;
	term.c.y = 5;
	term.mode |= MODE_ALTSCREEN;

	/* Put some content in history so it would scroll if allowed */
	term.histi = 5;
	for (int i = 1; i <= 5; i++)
		mock_set_hist(i, "old history line");

	vimnav_force_enter();

	/* Move to top of screen */
	for (int i = 0; i < 5; i++)
		vimnav_handle_key('k', 0);
	ASSERT_EQ(0, vimnav.y);
	ASSERT_EQ(0, term.scr);

	/* k at row 0 should NOT scroll into history on altscreen */
	vimnav_handle_key('k', 0);
	ASSERT_EQ(0, vimnav.y);
	ASSERT_EQ(0, term.scr);  /* No scroll happened */

	vimnav_exit();
	mock_term_free();
}

/* Test: L goes to bottom of screen on altscreen in forced mode */
TEST(vimnav_forced_L_bottom_of_screen_altscreen)
{
	mock_term_init(24, 80);
	for (int i = 0; i < 24; i++)
		mock_set_line(i, "content line");
	term.c.x = 0;
	term.c.y = 10;  /* TUI cursor in middle */
	term.mode |= MODE_ALTSCREEN;

	vimnav_force_enter();
	ASSERT_EQ(10, vimnav.y);

	/* L should go to bottom of screen, not TUI cursor row */
	vimnav_handle_key('L', 0);
	ASSERT_EQ(23, vimnav.y);

	vimnav_exit();
	mock_term_free();
}

/* Test: M goes to middle of full screen on altscreen in forced mode */
TEST(vimnav_forced_M_middle_of_screen_altscreen)
{
	mock_term_init(24, 80);
	for (int i = 0; i < 24; i++)
		mock_set_line(i, "content line");
	term.c.x = 0;
	term.c.y = 10;  /* TUI cursor in middle */
	term.mode |= MODE_ALTSCREEN;

	vimnav_force_enter();

	/* M should go to middle between H(0) and L(23) = 11 */
	vimnav_handle_key('M', 0);
	ASSERT_EQ(11, vimnav.y);  /* (term.row - 1) / 2 = 23 / 2 = 11 */

	vimnav_exit();
	mock_term_free();
}

/* Test: visual mode and yank work in forced mode */
TEST(vimnav_forced_visual_yank_works)
{
	mock_term_init(24, 80);
	mock_set_line(10, "hello world test");
	term.c.x = 0;
	term.c.y = 10;

	vimnav_force_enter();
	vimnav.x = 6;  /* on 'w' */
	vimnav.savedx = 6;

	/* Enter visual mode */
	vimnav_handle_key('v', 0);
	ASSERT_EQ(2, vimnav.mode);  /* VIMNAV_VISUAL */

	/* Select word forward */
	vimnav_handle_key('e', 0);
	ASSERT_EQ(10, vimnav.x);  /* end of "world" */

	/* Yank — don't mock_reset() since it would clear sel state */
	int prev_xsetsel = mock_state.xsetsel_calls;
	vimnav_handle_key('y', 0);
	ASSERT_EQ(1, vimnav.mode);  /* Back to VIMNAV_NORMAL */
	ASSERT(mock_state.xsetsel_calls > prev_xsetsel);  /* Text was yanked */

	vimnav_exit();
	mock_term_free();
}

/* Test: prompt space always returns 0 in forced mode */
TEST(vimnav_forced_no_prompt_space)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% echo hello");  /* Looks like a prompt */
	term.c.x = 5;
	term.c.y = 10;
	term.scr = 0;

	vimnav_force_enter();

	/* h/l should be handled internally (not passed to zsh) */
	vimnav.x = 5;
	vimnav.savedx = 5;
	int handled = vimnav_handle_key('h', 0);
	ASSERT_EQ(1, handled);  /* Handled by st */
	ASSERT_EQ(4, vimnav.x);

	handled = vimnav_handle_key('l', 0);
	ASSERT_EQ(1, handled);  /* Handled by st */
	ASSERT_EQ(5, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: { jumps to previous prompt on screen */
TEST(vimnav_open_brace_jumps_to_prev_prompt)
{
	mock_term_init(24, 80);
	mock_set_line(5, "$ ls");
	mock_set_line(6, "file1.txt");
	mock_set_line(7, "file2.txt");
	mock_set_line(10, "$ echo hello");
	mock_set_line(11, "hello");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 11;  /* On "hello" output line */
	vimnav.x = 3;
	vimnav.savedx = 3;

	int handled = vimnav_handle_key('{', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(10, vimnav.y);  /* Jumped to "$ echo hello" */
	ASSERT_EQ(0, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: { skips output lines to reach prompt */
TEST(vimnav_open_brace_skips_output_lines)
{
	mock_term_init(24, 80);
	mock_set_line(2, "$ first-cmd");
	mock_set_line(3, "output1");
	mock_set_line(4, "output2");
	mock_set_line(5, "output3");
	mock_set_line(6, "output4");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 6;
	vimnav.x = 0;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('{', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(2, vimnav.y);  /* Jumped to "$ first-cmd" */

	vimnav_exit();
	mock_term_free();
}

/* Test: { from one prompt jumps to the previous prompt */
TEST(vimnav_open_brace_from_prompt_to_prev_prompt)
{
	mock_term_init(24, 80);
	mock_set_line(3, "$ first");
	mock_set_line(4, "output");
	mock_set_line(5, "$ second");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 5;  /* On "$ second" prompt */
	vimnav.x = 3;
	vimnav.savedx = 3;

	int handled = vimnav_handle_key('{', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(3, vimnav.y);  /* Jumped to "$ first" */
	ASSERT_EQ(0, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: { with no prompt above goes to top (row 0) */
TEST(vimnav_open_brace_no_prev_prompt_goes_to_top)
{
	mock_term_init(24, 80);
	mock_set_line(0, "some output");
	mock_set_line(1, "more output");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 1;
	vimnav.x = 0;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('{', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, vimnav.y);  /* Top of screen */

	vimnav_exit();
	mock_term_free();
}

/* Test: { from current prompt jumps to previous prompt in history */
TEST(vimnav_open_brace_from_current_prompt)
{
	mock_term_init(24, 80);
	mock_set_line(10, "$ old-cmd");
	mock_set_line(11, "output");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	ASSERT_EQ(20, vimnav.y);

	int handled = vimnav_handle_key('{', 0);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(10, vimnav.y);  /* Jumped to "$ old-cmd" */
	ASSERT_EQ(0, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: } jumps to next prompt on screen */
TEST(vimnav_close_brace_jumps_to_next_prompt)
{
	mock_term_init(24, 80);
	mock_set_line(2, "$ first");
	mock_set_line(3, "output1");
	mock_set_line(4, "output2");
	mock_set_line(8, "$ second");
	mock_set_line(9, "output3");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 3;  /* On "output1" */
	vimnav.x = 2;
	vimnav.savedx = 2;

	int handled = vimnav_handle_key('}', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(8, vimnav.y);  /* Jumped to "$ second" */
	ASSERT_EQ(0, vimnav.x);

	vimnav_exit();
	mock_term_free();
}

/* Test: } with no next prompt goes to current prompt (like G) */
TEST(vimnav_close_brace_no_next_prompt_goes_to_current)
{
	mock_term_init(24, 80);
	mock_set_line(5, "$ cmd");
	mock_set_line(6, "output");
	mock_set_line(7, "more output");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 3;

	vimnav_enter();
	vimnav.y = 6;
	vimnav.x = 0;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('}', 0);

	ASSERT_EQ(1, handled);
	ASSERT_EQ(20, vimnav.y);  /* Jumped to current prompt */

	vimnav_exit();
	mock_term_free();
}

/* Test: Ctrl+E at bottom (term.scr == 0) should not move cursor */
TEST(vimnav_ctrl_e_no_scroll_no_cursor_move)
{
	mock_term_init(24, 80);
	mock_set_line(20, "% echo hello");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;

	vimnav_enter();
	/* Cursor on prompt line */
	ASSERT_EQ(20, vimnav.y);
	int old_y = vimnav.y;
	int old_x = vimnav.x;

	/* Ctrl+E when fully scrolled down - should do nothing */
	int handled = vimnav_handle_key('e', 4);  /* 4 = ControlMask */
	ASSERT_EQ(1, handled);
	ASSERT_EQ(old_y, vimnav.y);
	ASSERT_EQ(old_x, vimnav.x);
	ASSERT_EQ(0, term.scr);

	vimnav_exit();
	mock_term_free();
}

/* Test: Ctrl+Y at top of history should not move cursor */
TEST(vimnav_ctrl_y_no_scroll_no_cursor_move)
{
	mock_term_init(24, 80);

	/* No history content at all */
	term.histi = 0;
	mock_set_line(23, "% prompt");

	term.c.x = 0;
	term.c.y = 23;
	term.scr = 0;

	vimnav_enter();
	vimnav.y = 0;
	vimnav.x = 0;
	vimnav.savedx = 0;

	/* Ctrl+Y when no history to scroll into - should do nothing */
	int handled = vimnav_handle_key('y', 4);  /* 4 = ControlMask */
	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, vimnav.y);
	ASSERT_EQ(0, vimnav.x);
	ASSERT_EQ(0, term.scr);

	vimnav_exit();
	mock_term_free();
}

/* Test: Ctrl+1 jumps to top of screen (0%) */
TEST(vimnav_ctrl1_jumps_to_top)
{
	mock_term_init(24, 80);
	mock_set_line(0, "top line");
	mock_set_line(10, "middle line");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 10;
	vimnav.x = 5;
	vimnav.savedx = 5;

	int handled = vimnav_handle_key('1', 4);  /* 4 = ControlMask */
	ASSERT_EQ(1, handled);
	ASSERT_EQ(0, vimnav.y);  /* 0% = top */

	vimnav_exit();
	mock_term_free();
}

/* Test: Ctrl+6 jumps to middle of screen (50%) */
TEST(vimnav_ctrl6_jumps_to_middle)
{
	mock_term_init(24, 80);
	mock_set_line(0, "top line");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 0;
	vimnav.x = 0;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('6', 4);  /* 4 = ControlMask */
	ASSERT_EQ(1, handled);
	/* 50% of screen (23 rows) = 12, prompt at 20 so no clamping */
	ASSERT_EQ(12, vimnav.y);

	vimnav_exit();
	mock_term_free();
}

/* Test: Ctrl+- jumps to bottom of screen (100%) */
TEST(vimnav_ctrl_minus_jumps_to_bottom)
{
	mock_term_init(24, 80);
	mock_set_line(0, "top line");
	mock_set_line(20, "% prompt");

	term.c.x = 5;
	term.c.y = 20;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 0;
	vimnav.x = 0;
	vimnav.savedx = 0;

	int handled = vimnav_handle_key('-', 4);  /* 4 = ControlMask */
	ASSERT_EQ(1, handled);
	ASSERT_EQ(20, vimnav.y);  /* 100% = prompt row */

	vimnav_exit();
	mock_term_free();
}

/* Test: Ctrl+percent uses prompt row as bottom when not scrolled */
TEST(vimnav_ctrl_percent_respects_prompt)
{
	mock_term_init(24, 80);
	mock_set_line(10, "% prompt");

	term.c.x = 5;
	term.c.y = 10;  /* Prompt at row 10 */
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 0;
	vimnav.x = 0;
	vimnav.savedx = 0;

	/* Ctrl+6 = 50% of screen (23) = 12, clamped to prompt at row 10 */
	int handled = vimnav_handle_key('6', 4);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(10, vimnav.y);

	vimnav_exit();
	mock_term_free();
}

/* Test: Ctrl+percent uses full screen when scrolled up past prompt */
TEST(vimnav_ctrl_percent_scrolled_uses_full_screen)
{
	mock_term_init(24, 80);
	for (int i = 0; i < 24; i++)
		mock_set_line(i, "line content");

	term.c.x = 5;
	term.c.y = 23;  /* Prompt at bottom */
	term.scr = 10;  /* Scrolled up - prompt off screen */
	vimnav.zsh_cursor = 0;

	vimnav_enter();
	vimnav.y = 0;
	vimnav.x = 0;
	vimnav.savedx = 0;

	/* Ctrl+- = 100%, bottom = min(23+10, 23) = 23 */
	int handled = vimnav_handle_key('-', 4);
	ASSERT_EQ(1, handled);
	ASSERT_EQ(23, vimnav.y);  /* Bottom of visible screen */

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
	RUN_TEST(vimnav_editing_keys_snap_to_prompt);
	RUN_TEST(vimnav_JK_snap_to_prompt);
	RUN_TEST(vimnav_inherits_zsh_visual);
	RUN_TEST(vimnav_visual_handoff_on_k);
	RUN_TEST(vimnav_escape_clears_zsh_visual);
	RUN_TEST(vimnav_cursor_sync_on_return_to_prompt);
	RUN_TEST(vimnav_cursor_sync_repeated_j_in_prompt_space);
	RUN_TEST(vimnav_visual_mode_defers_cursor_sync);
	RUN_TEST(vimnav_yank_clears_zsh_visual);
	RUN_TEST(vimnav_v_toggle_off_notifies_zsh);
	RUN_TEST(vimnav_V_toggle_off_notifies_zsh);
	RUN_TEST(vimnav_hl_works_on_history_with_empty_prompt);
	RUN_TEST(vimnav_e_moves_to_word_end);
	RUN_TEST(vimnav_W_moves_to_next_WORD);
	RUN_TEST(vimnav_B_moves_to_prev_WORD);
	RUN_TEST(vimnav_E_moves_to_WORD_end);
	RUN_TEST(vimnav_eEWB_prompt_passthrough);
	/* Text object tests */
	RUN_TEST(vimnav_viw_selects_inner_word);
	RUN_TEST(vimnav_vaw_selects_around_word);
	RUN_TEST(vimnav_viW_selects_inner_WORD);
	RUN_TEST(vimnav_vi_quote_selects_inner_quotes);
	RUN_TEST(vimnav_va_quote_selects_around_quotes);
	RUN_TEST(vimnav_vi_paren_selects_inner_parens);
	RUN_TEST(vimnav_vi_close_paren_selects_inner_parens);
	RUN_TEST(vimnav_vi_brace_selects_inner_braces);
	RUN_TEST(vimnav_vi_bracket_selects_inner_brackets);
	RUN_TEST(vimnav_text_objects_prompt_passthrough);
	RUN_TEST(vimnav_vi_paren_nested);
	RUN_TEST(vimnav_vib_selects_inner_parens);
	RUN_TEST(vimnav_viB_selects_inner_braces);
	RUN_TEST(vimnav_textobj_unknown_key_clears_pending);
	/* Search-right text object tests */
	RUN_TEST(vimnav_vi_paren_searches_right);
	RUN_TEST(vimnav_vi_paren_no_closing);
	RUN_TEST(vimnav_vi_paren_pair_to_left);
	RUN_TEST(vimnav_vi_quote_searches_right);
	RUN_TEST(vimnav_vi_bracket_searches_right);
	RUN_TEST(vimnav_va_paren_searches_right);
	RUN_TEST(vimnav_ia_normal_mode_snaps_to_prompt);
	/* Scrollback after clear tests */
	RUN_TEST(vimnav_scroll_up_after_clear);
	RUN_TEST(vimnav_ctrl_u_after_clear);
	RUN_TEST(vimnav_scroll_stops_at_empty_history);
	RUN_TEST(vimnav_scroll_reaches_oldest_history);
	/* f/F find character tests */
	RUN_TEST(vimnav_f_finds_char_forward);
	RUN_TEST(vimnav_F_finds_char_backward);
	RUN_TEST(vimnav_f_no_match_stays_put);
	RUN_TEST(vimnav_semicolon_repeats_f_forward);
	RUN_TEST(vimnav_comma_repeats_f_backward);
	RUN_TEST(vimnav_semicolon_repeats_F_backward);
	RUN_TEST(vimnav_comma_repeats_F_forward);
	RUN_TEST(vimnav_fF_prompt_passthrough);
	/* H/M/L screen navigation tests */
	RUN_TEST(vimnav_H_moves_to_screen_top);
	RUN_TEST(vimnav_L_moves_to_screen_bottom);
	RUN_TEST(vimnav_L_stays_on_screen_when_scrolled);
	RUN_TEST(vimnav_M_moves_to_middle);
	RUN_TEST(vimnav_M_middle_when_scrolled);
	RUN_TEST(vimnav_L_scrolled_prompt_visible);
	RUN_TEST(vimnav_M_scrolled_prompt_visible);
	/* gg/G navigation tests */
	RUN_TEST(vimnav_gg_moves_to_top);
	RUN_TEST(vimnav_g_nonmatch_clears_pending);
	RUN_TEST(vimnav_G_moves_to_prompt);
	RUN_TEST(vimnav_gg_inherits_zsh_visual);
	/* vimnav_curline_y tests */
	RUN_TEST(vimnav_curline_y_inactive);
	RUN_TEST(vimnav_curline_y_prompt_space);
	RUN_TEST(vimnav_curline_y_history);
	RUN_TEST(vimnav_curline_y_visual_mode);
	RUN_TEST(vimnav_curline_y_below_cursor);
	/* Empty line selection test */
	RUN_TEST(vimnav_visual_empty_line_selectable);
	/* Visual mode snap-to-prompt cleanup tests */
	RUN_TEST(vimnav_editing_keys_clear_visual_on_snap);
	RUN_TEST(vimnav_visual_mode_consumes_unknown_keys);
	/* Forced nav mode (Shift+Escape) tests */
	RUN_TEST(vimnav_force_enter_works_on_altscreen);
	RUN_TEST(vimnav_force_enter_no_double_entry);
	RUN_TEST(vimnav_upgrade_regular_to_forced);
	RUN_TEST(vimnav_forced_escape_stays);
	RUN_TEST(vimnav_forced_escape_clears_visual_first);
	RUN_TEST(vimnav_forced_i_exits);
	RUN_TEST(vimnav_forced_a_exits);
	RUN_TEST(vimnav_forced_editing_keys_noop);
	RUN_TEST(vimnav_forced_navigation_works);
	RUN_TEST(vimnav_forced_j_below_cursor_altscreen);
	RUN_TEST(vimnav_forced_k_no_history_scroll_altscreen);
	RUN_TEST(vimnav_forced_L_bottom_of_screen_altscreen);
	RUN_TEST(vimnav_forced_M_middle_of_screen_altscreen);
	RUN_TEST(vimnav_forced_visual_yank_works);
	RUN_TEST(vimnav_forced_no_prompt_space);
	/* { and } prompt-jump tests */
	RUN_TEST(vimnav_open_brace_jumps_to_prev_prompt);
	RUN_TEST(vimnav_open_brace_skips_output_lines);
	RUN_TEST(vimnav_open_brace_from_prompt_to_prev_prompt);
	RUN_TEST(vimnav_open_brace_no_prev_prompt_goes_to_top);
	RUN_TEST(vimnav_open_brace_from_current_prompt);
	RUN_TEST(vimnav_close_brace_jumps_to_next_prompt);
	RUN_TEST(vimnav_close_brace_no_next_prompt_goes_to_current);

	RUN_TEST(vimnav_ctrl_e_no_scroll_no_cursor_move);
	RUN_TEST(vimnav_ctrl_y_no_scroll_no_cursor_move);
	/* Ctrl+1-9,0,- screen percent jump tests */
	RUN_TEST(vimnav_ctrl1_jumps_to_top);
	RUN_TEST(vimnav_ctrl6_jumps_to_middle);
	RUN_TEST(vimnav_ctrl_minus_jumps_to_bottom);
	RUN_TEST(vimnav_ctrl_percent_respects_prompt);
	RUN_TEST(vimnav_ctrl_percent_scrolled_uses_full_screen);
}

/* === Prompt line range tests === */

TEST(prompt_range_at_bottom)
{
	int start_y, end_y;

	mock_term_init(24, 80);
	mock_set_line(23, "% prompt");
	term.c.x = 5;
	term.c.y = 23;
	term.scr = 0;
	term.mode = 0;  /* no alt screen */

	vimnav_prompt_line_range(&start_y, &end_y);

	ASSERT_EQ(23, start_y);
	ASSERT_EQ(23, end_y);

	mock_term_free();
}

TEST(prompt_range_scrolled_off_screen)
{
	int start_y, end_y;

	/* Prompt at term.line[23], screen row = 23+5 = 28, off-screen (>= 24) */
	mock_term_init(24, 80);
	mock_set_line(23, "% prompt");
	term.c.x = 5;
	term.c.y = 23;
	term.scr = 5;  /* scrolled up, prompt pushed off screen */
	term.mode = 0;

	vimnav_prompt_line_range(&start_y, &end_y);

	ASSERT_EQ(-1, start_y);
	ASSERT_EQ(-1, end_y);

	mock_term_free();
}

TEST(prompt_range_altscreen)
{
	int start_y, end_y;

	mock_term_init(24, 80);
	mock_set_line(23, "% prompt");
	term.c.x = 5;
	term.c.y = 23;
	term.scr = 0;
	term.mode = (1 << 2);  /* MODE_ALTSCREEN */

	vimnav_prompt_line_range(&start_y, &end_y);

	ASSERT_EQ(-1, start_y);
	ASSERT_EQ(-1, end_y);

	mock_term_free();
}

TEST(prompt_range_scrolled_still_visible)
{
	int start_y, end_y;

	/* After Ctrl+L: prompt at term.line[0], scrolled up 3 lines.
	 * Screen row = 0+3 = 3, still visible (< 24).
	 * This was the bug: prompt was incorrectly reported as not visible
	 * whenever term.scr != 0, even if the prompt was still on screen. */
	mock_term_init(24, 80);
	mock_set_line(0, "% prompt");
	term.c.x = 5;
	term.c.y = 0;
	term.scr = 3;
	term.mode = 0;

	vimnav_prompt_line_range(&start_y, &end_y);

	ASSERT_EQ(3, start_y);  /* prompt's screen row */
	ASSERT_EQ(3, end_y);

	mock_term_free();
}

TEST(prompt_range_scrolled_just_barely_visible)
{
	int start_y, end_y;

	/* Prompt at term.line[0], scrolled up 23 lines.
	 * Screen row = 0+23 = 23, last visible row. */
	mock_term_init(24, 80);
	mock_set_line(0, "% prompt");
	term.c.x = 5;
	term.c.y = 0;
	term.scr = 23;
	term.mode = 0;

	vimnav_prompt_line_range(&start_y, &end_y);

	ASSERT_EQ(23, start_y);
	ASSERT_EQ(23, end_y);

	mock_term_free();
}

TEST(prompt_range_scrolled_just_off_screen)
{
	int start_y, end_y;

	/* Prompt at term.line[0], scrolled up 24 lines.
	 * Screen row = 0+24 = 24 >= 24, off screen. */
	mock_term_init(24, 80);
	mock_set_line(0, "% prompt");
	term.c.x = 5;
	term.c.y = 0;
	term.scr = 24;
	term.mode = 0;

	vimnav_prompt_line_range(&start_y, &end_y);

	ASSERT_EQ(-1, start_y);
	ASSERT_EQ(-1, end_y);

	mock_term_free();
}

TEST(prompt_range_scrolled_mid_screen)
{
	int start_y, end_y;

	/* Prompt at term.line[5], scrolled up 3 lines.
	 * Screen row = 5+3 = 8, still visible. */
	mock_term_init(24, 80);
	mock_set_line(5, "% prompt");
	term.c.x = 5;
	term.c.y = 5;
	term.scr = 3;
	term.mode = 0;

	vimnav_prompt_line_range(&start_y, &end_y);

	ASSERT_EQ(8, start_y);
	ASSERT_EQ(8, end_y);

	mock_term_free();
}

/* === Yank trailing newline tests === */

/* Test: yy on history line does NOT include trailing newline */
TEST(vimnav_yy_history_no_trailing_newline)
{
	mock_term_init(24, 80);
	mock_set_line(9, "hello world");
	mock_set_line(23, "% prompt");

	term.c.x = 5;
	term.c.y = 23;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();

	/* Move cursor to history line (not prompt) */
	vimnav.x = 0;
	vimnav.y = 9;

	/* Single 'y' in normal mode with no selection calls vimnav_yank_line() */
	mock_reset();
	vimnav_handle_key('y', 0);

	/* Should have yanked without trailing newline */
	ASSERT(mock_state.xsetsel_calls > 0);
	ASSERT_STR_EQ("hello world", mock_state.last_xsetsel);

	vimnav_exit();
	mock_term_free();
}

/* Test: V+y on single line does NOT include trailing newline */
TEST(vimnav_Vy_single_line_no_trailing_newline)
{
	mock_term_init(24, 80);
	mock_set_line(9, "single line text");
	mock_set_line(23, "% prompt");

	term.c.x = 5;
	term.c.y = 23;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();

	/* Move cursor to history line */
	vimnav.x = 0;
	vimnav.y = 9;

	/* Press V to enter visual line, then y to yank */
	mock_reset();
	vimnav_handle_key('V', 1);  /* 1 = ShiftMask */
	vimnav_handle_key('y', 0);

	/* Should have yanked without trailing newline */
	ASSERT(mock_state.xsetsel_calls > 0);
	ASSERT_STR_EQ("single line text", mock_state.last_xsetsel);

	vimnav_exit();
	mock_term_free();
}

/* Test: V+jy on multiple lines DOES keep newlines */
TEST(vimnav_Vjy_multi_line_keeps_newlines)
{
	mock_term_init(24, 80);
	mock_set_line(9, "first line");
	mock_set_line(10, "second line");
	mock_set_line(23, "% prompt");

	term.c.x = 5;
	term.c.y = 23;
	term.scr = 0;
	vimnav.zsh_cursor = 0;

	vimnav_enter();

	/* Move cursor to history line */
	vimnav.x = 0;
	vimnav.y = 9;

	/* V, j, y to yank two lines */
	mock_reset();
	vimnav_handle_key('V', 1);  /* 1 = ShiftMask */
	vimnav_handle_key('j', 0);
	vimnav_handle_key('y', 0);

	/* Multi-line should keep newlines */
	ASSERT(mock_state.xsetsel_calls > 0);
	ASSERT_STR_EQ("first line\nsecond line\n", mock_state.last_xsetsel);

	vimnav_exit();
	mock_term_free();
}

TEST_SUITE(yank_newline)
{
	RUN_TEST(vimnav_yy_history_no_trailing_newline);
	RUN_TEST(vimnav_Vy_single_line_no_trailing_newline);
	RUN_TEST(vimnav_Vjy_multi_line_keeps_newlines);
}

TEST_SUITE(prompt_range)
{
	RUN_TEST(prompt_range_at_bottom);
	RUN_TEST(prompt_range_scrolled_off_screen);
	RUN_TEST(prompt_range_altscreen);
	RUN_TEST(prompt_range_scrolled_still_visible);
	RUN_TEST(prompt_range_scrolled_just_barely_visible);
	RUN_TEST(prompt_range_scrolled_just_off_screen);
	RUN_TEST(prompt_range_scrolled_mid_screen);
}

int
main(void)
{
	printf("st test suite\n");
	printf("========================================\n");

	RUN_SUITE(vimnav);
	RUN_SUITE(yank_newline);
	RUN_SUITE(prompt_range);

	return test_summary();
}
