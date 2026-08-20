/* See LICENSE for license details. */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "st.h"
#include "vimnav.h"

/* X11 modifier masks (from X11/X.h) */
#define ShiftMask   (1<<0)
#define ControlMask (1<<2)

/* Access to st.c internals */
#define HISTSIZE      (1 << 15)
#define IS_SET(flag)  ((term.mode & (flag)) != 0)
#define ISDELIM(u)    (u && wcschr(worddelimiters, u))
#define TLINE(y)      ((y) < term.scr ? term.hist[((y) + term.histi - \
                      term.scr + HISTSIZE + 1) % HISTSIZE] : \
                      term.line[(y) - term.scr])

/* Vim navigation mode states (internal use) */
enum vimnav_mode {
	VIMNAV_INACTIVE = 0,
	VIMNAV_NORMAL = 1,
	VIMNAV_VISUAL = 2,
	VIMNAV_VISUAL_LINE = 3,
	VIMNAV_VISUAL_BLOCK = 4,
};

/* Terminal mode flags from st.c */
enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_INSERT      = 1 << 1,
	MODE_ALTSCREEN   = 1 << 2,
	MODE_CRLF        = 1 << 3,
	MODE_ECHO        = 1 << 4,
	MODE_PRINT       = 1 << 5,
	MODE_UTF8        = 1 << 6,
};

/* Cursor structure */
typedef struct {
	Glyph attr;
	int x;
	int y;
	char state;
} TCursor;

/* Selection structure */
typedef struct {
	int mode;
	int type;
	int snap;
	struct {
		int x, y;
	} nb, ne, ob, oe;
	int alt;
} Selection;

/* Terminal structure */
typedef struct {
	int row;
	int col;
	int maxcol;
	Line *line;
	Line *alt;
	Line hist[HISTSIZE];
	int histi;
	int scr;
	int *dirty;
	TCursor c;
	int ocx;
	int ocy;
	int top;
	int bot;
	int mode;
	int esc;
	char trantbl[4];
	int charset;
	int icharset;
	int *tabs;
	Rune lastc;
	int histn;
} Term;

/* Extern declarations for st.c globals */
extern Term term;
extern Selection sel;
extern wchar_t *worddelimiters;

/* Extern declarations for functions in st.c */
extern int tlinelen(int y);
extern void tfulldirt(void);

/* Extern declarations for functions in x.c */
extern void xsetsel(char *str);
extern void xsetimage(unsigned char *png, size_t length);
extern void xclipcopy(void);
extern void clippaste(const Arg *);

/* Extern declarations for cmdline.c */
extern void cmdline_open(void);

/* Globals - exported via vimnav.h */
VimNav vimnav = { .mode = VIMNAV_INACTIVE };
static int vimnav_paste_strip_newlines = 0;
static int vimnav_paste_after_cursor = 0;  /* Send Escape after paste completes */

/* Forward declarations */
static int vimnav_find_prompt_end(int screen_y);
static int vimnav_find_prompt_start_y(void);
static int vimnav_is_prompt_space(int y);
static void vimnav_update_selection(void);
static void vimnav_notify_zsh_visual_end(void);
static void vimnav_sync_to_zsh_cursor(void);
static int vimnav_has_main_prompt(int screen_y);

/* Notify zsh that visual mode selection has ended (send Escape) */
static void
vimnav_notify_zsh_visual_end(void)
{
	if (vimnav.zsh_visual) {
		/* Send Escape to zsh to exit visual mode */
		ttywrite("\033", 1, 1);
		vimnav.zsh_visual = 0;
	}
}

/* Sync st cursor to zsh cursor position (call when returning to prompt space) */
static void
vimnav_sync_to_zsh_cursor(void)
{
	if (vimnav_is_prompt_space(vimnav.y)) {
		int prompt_end = vimnav_find_prompt_end(vimnav.y);
		vimnav.x = prompt_end + vimnav.zsh_cursor;
		vimnav.savedx = vimnav.x;
		vimnav.last_shell_x = vimnav.x;
	}
}

/* Check if currently in any visual mode (char, line, or block) */
static int
vimnav_is_visual(void)
{
	return vimnav.mode >= VIMNAV_VISUAL;
}

/* Ctrl+V block selection deliberately borrows forced-nav semantics while it
 * is active: st, rather than zsh, owns the cursor and the selected region. */
int
vimnav_terminal_owned(void)
{
	return vimnav.forced || vimnav.mode == VIMNAV_VISUAL_BLOCK;
}

/* Snap back to prompt line (scroll down if needed, update cursor position) */
static void
vimnav_snap_to_prompt(void)
{
	/* Clear visual mode and selection before returning to prompt */
	if (vimnav_is_visual()) {
		vimnav.mode = VIMNAV_NORMAL;
		vimnav_notify_zsh_visual_end();
		selclear();
	}
	if (term.scr > 0) {
		kscrolldown(&(Arg){ .i = term.scr });
	}
	vimnav.y = term.c.y;
	vimnav_sync_to_zsh_cursor();
	tfulldirt();
}

/* zsh cursor/visual sync functions */
void
vimnav_set_zsh_cursor(int pos)
{
	int prompt_end;

	vimnav.zsh_cursor = pos;
	if (vimnav_terminal_owned())
		return;

	/* If in nav mode on prompt line, update our cursor to match zsh */
	if (vimnav.mode != VIMNAV_INACTIVE && term.scr == 0 && vimnav.y == term.c.y) {
		prompt_end = vimnav_find_prompt_end(vimnav.y);
		vimnav.x = prompt_end + pos;
		vimnav.savedx = vimnav.x;
		vimnav.last_shell_x = vimnav.x;
		if (vimnav_is_visual()) {
			vimnav_update_selection();
		}
		tfulldirt();
	}

	/* If zsh is in visual mode on prompt line, update st's selection rendering */
	if (vimnav.zsh_visual && vimnav.mode != VIMNAV_INACTIVE &&
	    term.scr == 0 && vimnav.y == term.c.y && vimnav.mode == VIMNAV_NORMAL) {
		prompt_end = vimnav_find_prompt_end(term.c.y);
		int anchor_x = prompt_end + vimnav.zsh_visual_anchor;
		int cursor_x = prompt_end + pos;
		if (vimnav.zsh_visual_line) {
			selstart(0, term.c.y, 0);
			sel.snap = SNAP_LINE;
			selextend(term.col - 1, term.c.y, SEL_REGULAR, 0);
		} else {
			selstart(anchor_x, term.c.y, 0);
			selextend(cursor_x, term.c.y, SEL_REGULAR, 0);
		}
		tfulldirt();
	}
}

void
vimnav_set_zsh_visual(int active, int anchor, int line_mode)
{
	int prompt_end;

	vimnav.zsh_visual = active;
	vimnav.zsh_visual_anchor = anchor;
	vimnav.zsh_visual_line = line_mode;

	/* Forced nav and Ctrl+V block mode are wholly rendered and controlled by
	 * st. Keep the reported state for later, but a delayed shell report must
	 * not move or cancel the terminal-owned selection. */
	if (vimnav_terminal_owned())
		return;

	if (active && vimnav.mode != VIMNAV_INACTIVE && term.scr == 0) {
		/* zsh entered visual mode on prompt line - st renders the selection */
		prompt_end = vimnav_find_prompt_end(term.c.y);
		int anchor_x = prompt_end + anchor;
		int cursor_x = prompt_end + vimnav.zsh_cursor;
		if (line_mode) {
			selstart(0, term.c.y, 0);
			sel.snap = SNAP_LINE;
			selextend(term.col - 1, term.c.y, SEL_REGULAR, 0);
		} else {
			selstart(anchor_x, term.c.y, 0);
			selextend(cursor_x, term.c.y, SEL_REGULAR, 0);
		}
		tfulldirt();
	} else if (!active && vimnav.mode != VIMNAV_INACTIVE) {
		/* zsh exited visual mode - clear selection */
		if (vimnav_is_visual()) {
			vimnav.mode = VIMNAV_NORMAL;
		}
		selclear();
		tfulldirt();
	}
}

int
tisvimnav(void)
{
	return vimnav.mode != VIMNAV_INACTIVE;
}

int
vimnav_curline_y(void)
{
	/* Returns the y coordinate of the current line to highlight,
	 * or -1 if no line should be highlighted.
	 * Only highlight when in normal nav mode (not visual), NOT in prompt space,
	 * and zsh is not in visual mode. */
	if (vimnav.mode != VIMNAV_NORMAL)
		return -1;
	if (vimnav.zsh_visual)
		return -1;
	if (vimnav_is_prompt_space(vimnav.y))
		return -1;
	/* Don't highlight rows below prompt (empty space) */
	if (vimnav.y > term.c.y + term.scr)
		return -1;
	return vimnav.y;
}

void
vimnav_prompt_line_range(int *start_y, int *end_y)
{
	/* Returns the screen row range of the prompt space, or -1/-1 if
	 * not visible (scrolled off screen or alt screen).
	 * The prompt is at screen row (term.c.y + term.scr). */
	int prompt_end_y = term.c.y + term.scr;

	if (IS_SET(MODE_ALTSCREEN) || prompt_end_y >= term.row) {
		*start_y = *end_y = -1;
		return;
	}
	*start_y = vimnav_find_prompt_start_y();
	*end_y = prompt_end_y;
}

static int
vimnav_screen_y(void)
{
	return vimnav.y;
}

static void
vimnav_update_selection(void)
{
	int screen_y = vimnav_screen_y();
	int anchor_screen_y = vimnav.anchor_abs_y + term.scr;

	if (vimnav.mode == VIMNAV_VISUAL) {
		selstart(vimnav.anchor_x, anchor_screen_y, 0);
		selextend(vimnav.x, screen_y, SEL_REGULAR, 0);
	} else if (vimnav.mode == VIMNAV_VISUAL_LINE) {
		selstart(0, anchor_screen_y, 0);
		sel.snap = SNAP_LINE;
		selextend(term.col - 1, screen_y, SEL_REGULAR, 0);
	} else if (vimnav.mode == VIMNAV_VISUAL_BLOCK) {
		selstart(vimnav.anchor_x, anchor_screen_y, 0);
		/* selstart() defaults to regular selection; set the type before
		 * normalization so upward rectangles keep ordered x bounds. */
		sel.type = SEL_RECTANGULAR;
		selextend(vimnav.x, screen_y, SEL_RECTANGULAR, 0);
	}
	tfulldirt();
}

/* Rectangular selections use virtual columns so their cursor can cross short
 * lines without collapsing the selected width. Other modes stay text-bound. */
static int
vimnav_max_x_for_line(int y)
{
	int linelen;

	if (vimnav.mode == VIMNAV_VISUAL_BLOCK)
		return MAX(term.col - 1, 0);

	linelen = tlinelen(y);
	return linelen > 0 ? linelen - 1 : 0;
}

static void
vimnav_restore_saved_column(void)
{
	vimnav.x = MIN(vimnav.savedx, vimnav_max_x_for_line(vimnav_screen_y()));
	if (vimnav.x < 0)
		vimnav.x = 0;
}

static void
vimnav_move_left(void)
{
	if (vimnav.x > 0) {
		vimnav.x--;
		vimnav.savedx = vimnav.x;
	}
	vimnav_update_selection();
}

static void
vimnav_move_right(void)
{
	int max_x = vimnav_max_x_for_line(vimnav_screen_y());

	if (vimnav.x < max_x) {
		vimnav.x++;
		vimnav.savedx = vimnav.x;
	}
	vimnav_update_selection();
}

/* Scroll helper that respects vim nav boundaries and moves cursor */
static void
vimnav_scroll_up(int n)
{
	int old_scr = term.scr;
	int available = MAX(0, term.histn - term.scr);
	int scrolled = MIN(n, available);
	int remaining;

	/* Every populated history row is navigable. Image placements are anchored
	 * to otherwise blank rows, so inspecting glyph text to decide whether a row
	 * exists incorrectly makes image-only scrollback unreachable. */
	if (scrolled > 0)
		kscrollup(&(Arg){ .i = scrolled });
	scrolled = term.scr - old_scr;

	/* Move cursor up by the amount we couldn't scroll */
	remaining = n - scrolled;
	if (remaining > 0 && vimnav.y > 0) {
		vimnav.y -= remaining;
		if (vimnav.y < 0)
			vimnav.y = 0;
	}

	vimnav_restore_saved_column();

	/* Update selection if in visual mode */
	vimnav_update_selection();
}

static void
vimnav_scroll_down(int n)
{
	int scrolled;
	int max_scroll;
	int requested = n;  /* Save original request before capping */

	/* Don't scroll past where the prompt currently is */
	max_scroll = term.scr;  /* Can scroll down at most term.scr lines */
	if (n > max_scroll)
		n = max_scroll;

	scrolled = n;
	if (scrolled > 0) {
		kscrolldown(&(Arg){ .i = scrolled });
	}

	/* Move cursor down by the amount we couldn't scroll */
	int remaining = requested - scrolled;
	if (remaining > 0) {
		/* Can move cursor down to prompt position (term.scr + term.c.y) */
		int max_valid_y = term.scr + term.c.y;
		if (vimnav.y < max_valid_y) {
			vimnav.y += remaining;
			if (vimnav.y > max_valid_y)
				vimnav.y = max_valid_y;
		}
	}

	/* Clamp cursor to prompt if we ended up past it */
	int max_valid_y = term.scr + term.c.y;
	if (vimnav.y > max_valid_y) {
		vimnav.y = max_valid_y;
		if (term.scr == 0) {
			vimnav.x = term.c.x;
			vimnav.last_shell_x = term.c.x;
		}
	}

	vimnav_restore_saved_column();

	/* Update selection if in visual mode */
	vimnav_update_selection();
}

static void
vimnav_move_up(void)
{
	int was_in_prompt_space = vimnav_is_prompt_space(vimnav.y);

	if (vimnav.y > 0) {
		/* Move cursor up within visible area */
		vimnav.y--;
	} else if (!IS_SET(MODE_ALTSCREEN)) {
		/* At top of screen, scroll into any valid history row, including a
		 * textually blank row occupied by a graphics placement. */
		if (term.scr < term.histn)
			kscrollup(&(Arg){ .i = 1 });
		/* Cursor stays at row 0 */
	}

	/* Handoff: if we left prompt space with zsh in visual mode, inherit selection */
	if (was_in_prompt_space && !vimnav_is_prompt_space(vimnav.y) &&
	    vimnav.zsh_visual && vimnav.mode == VIMNAV_NORMAL) {
		int prompt_screen_y = term.c.y + term.scr;
		int prompt_end = vimnav_find_prompt_end(prompt_screen_y);
		vimnav.anchor_x = prompt_end + vimnav.zsh_visual_anchor;
		vimnav.anchor_abs_y = term.c.y;  /* Anchor stays on prompt line */
		if (vimnav.zsh_visual_line) {
			vimnav.mode = VIMNAV_VISUAL_LINE;
		} else {
			vimnav.mode = VIMNAV_VISUAL;
		}
	}

	vimnav_restore_saved_column();
	vimnav_update_selection();
}

static void
vimnav_move_down(void)
{
	if (vimnav.forced && IS_SET(MODE_ALTSCREEN)) {
		/* Forced mode on alt screen: full screen is navigable */
		if (vimnav.y < term.row - 1)
			vimnav.y++;
	} else {
		/* The prompt is at screen row (term.scr + term.c.y) in scrolled view.
		 * Valid content is rows [0, term.scr + term.c.y]. Everything below is dead space. */
		int max_valid_y = term.scr + term.c.y;

		if (vimnav.y >= max_valid_y) {
			/* At or past prompt position, only allow scrolling down if scrolled */
			if (term.scr > 0) {
				kscrolldown(&(Arg){ .i = 1 });
				/* Recalculate after scroll and clamp if needed */
				max_valid_y = term.scr + term.c.y;
				if (vimnav.y > max_valid_y) {
					vimnav.y = max_valid_y;
					if (term.scr == 0) {
						vimnav.x = term.c.x;
						vimnav.last_shell_x = term.c.x;
					}
				}
			}
			/* If term.scr == 0, can't move down at all */
		} else if (vimnav.y >= term.row - 1) {
			/* At bottom of screen but not at prompt yet, scroll down */
			kscrolldown(&(Arg){ .i = 1 });
		} else {
			/* Can move down freely within valid range */
			vimnav.y++;
		}
	}

	/* If in prompt space in normal mode, sync to zsh cursor.
	 * This covers both entering prompt space from history AND staying
	 * in prompt space while scrolling down (e.g., repeated j after Ctrl+L).
	 * Don't sync during visual mode - wait until selection is done. */
	if (vimnav_is_prompt_space(vimnav.y) &&
	    vimnav.mode == VIMNAV_NORMAL) {
		int prompt_end = vimnav_find_prompt_end(vimnav.y);
		vimnav.x = prompt_end + vimnav.zsh_cursor;
		vimnav.savedx = vimnav.x;
		vimnav.last_shell_x = vimnav.x;
	} else {
		vimnav_restore_saved_column();
	}
	vimnav_update_selection();
}

static void
vimnav_move_bol(void)
{
	vimnav.x = 0;
	vimnav.savedx = 0;
	vimnav_update_selection();
}

static void
vimnav_move_eol(void)
{
	int linelen = tlinelen(vimnav_screen_y());
	vimnav.x = linelen > 0 ? linelen - 1 : 0;
	vimnav.savedx = vimnav.x;
	vimnav_update_selection();
}

static void
vimnav_move_word_forward(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	int linelen = tlinelen(screen_y);
	Glyph *gp;

	if (linelen == 0)
		return;

	/* Skip current word (non-delimiters) */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (ISDELIM(gp->u))
			break;
		x++;
	}

	/* Skip delimiters */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (!ISDELIM(gp->u))
			break;
		x++;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

static void
vimnav_move_word_backward(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	Glyph *gp;

	if (x == 0)
		return;

	x--;

	/* Skip delimiters */
	while (x > 0) {
		gp = &TLINE(screen_y)[x];
		if (!ISDELIM(gp->u))
			break;
		x--;
	}

	/* Skip to start of word */
	while (x > 0) {
		gp = &TLINE(screen_y)[x - 1];
		if (ISDELIM(gp->u))
			break;
		x--;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

static void
vimnav_move_word_end(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	int linelen = tlinelen(screen_y);
	Glyph *gp;

	if (linelen == 0 || x >= linelen - 1)
		return;

	/* Move forward at least one character */
	x++;

	/* Skip delimiters */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (!ISDELIM(gp->u))
			break;
		x++;
	}

	/* Move to end of word */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x + 1];
		if (ISDELIM(gp->u))
			break;
		x++;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

static void
vimnav_move_WORD_forward(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	int linelen = tlinelen(screen_y);
	Glyph *gp;

	if (linelen == 0)
		return;

	/* Skip current WORD (non-whitespace) */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (iswspace(gp->u))
			break;
		x++;
	}

	/* Skip whitespace */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (!iswspace(gp->u))
			break;
		x++;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

static void
vimnav_move_WORD_backward(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	Glyph *gp;

	if (x == 0)
		return;

	x--;

	/* Skip whitespace */
	while (x > 0) {
		gp = &TLINE(screen_y)[x];
		if (!iswspace(gp->u))
			break;
		x--;
	}

	/* Skip to start of WORD */
	while (x > 0) {
		gp = &TLINE(screen_y)[x - 1];
		if (iswspace(gp->u))
			break;
		x--;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

static void
vimnav_move_WORD_end(void)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	int linelen = tlinelen(screen_y);
	Glyph *gp;

	if (linelen == 0 || x >= linelen - 1)
		return;

	/* Move forward at least one character */
	x++;

	/* Skip whitespace */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x];
		if (!iswspace(gp->u))
			break;
		x++;
	}

	/* Move to end of WORD */
	while (x < linelen - 1) {
		gp = &TLINE(screen_y)[x + 1];
		if (iswspace(gp->u))
			break;
		x++;
	}

	vimnav.x = x;
	vimnav.savedx = x;
	vimnav_update_selection();
}

/* Find character on current line (f/F command)
 * forward: 1 for f (search right), 0 for F (search left)
 * Returns 1 if found and moved, 0 otherwise */
static int
vimnav_find_char(Rune c, int forward)
{
	int screen_y = vimnav_screen_y();
	int x = vimnav.x;
	int linelen = tlinelen(screen_y);
	Line line = TLINE(screen_y);
	int i;

	if (linelen == 0)
		return 0;

	if (forward) {
		/* Search forward from x+1 to end of line */
		for (i = x + 1; i < linelen; i++) {
			if (line[i].u == c) {
				vimnav.x = i;
				vimnav.savedx = i;
				vimnav_update_selection();
				return 1;
			}
		}
	} else {
		/* Search backward from x-1 to start of line */
		for (i = x - 1; i >= 0; i--) {
			if (line[i].u == c) {
				vimnav.x = i;
				vimnav.savedx = i;
				vimnav_update_selection();
				return 1;
			}
		}
	}

	return 0;  /* Character not found */
}

/* Text object selection helpers */

/* Find word boundaries around cursor position (inner = exclude delimiters) */
static int
vimnav_find_word_bounds(int x, int y, int inner, int *start_x, int *end_x)
{
	Line line = TLINE(y);
	int linelen = tlinelen(y);
	int sx, ex;
	Glyph *gp;

	if (linelen == 0 || x >= linelen)
		return 0;

	gp = &line[x];

	/* If on a delimiter, select the delimiter run (or whitespace for 'around') */
	if (ISDELIM(gp->u)) {
		sx = x;
		ex = x;
		/* Expand left */
		while (sx > 0 && ISDELIM(line[sx - 1].u))
			sx--;
		/* Expand right */
		while (ex < linelen - 1 && ISDELIM(line[ex + 1].u))
			ex++;
		*start_x = sx;
		*end_x = ex;
		return 1;
	}

	/* Find word start */
	sx = x;
	while (sx > 0 && !ISDELIM(line[sx - 1].u))
		sx--;

	/* Find word end */
	ex = x;
	while (ex < linelen - 1 && !ISDELIM(line[ex + 1].u))
		ex++;

	if (!inner) {
		/* 'around' - include trailing whitespace, or leading if at end */
		if (ex < linelen - 1 && ISDELIM(line[ex + 1].u)) {
			while (ex < linelen - 1 && ISDELIM(line[ex + 1].u))
				ex++;
		} else if (sx > 0 && ISDELIM(line[sx - 1].u)) {
			while (sx > 0 && ISDELIM(line[sx - 1].u))
				sx--;
		}
	}

	*start_x = sx;
	*end_x = ex;
	return 1;
}

/* Find WORD boundaries (whitespace-delimited) */
static int
vimnav_find_WORD_bounds(int x, int y, int inner, int *start_x, int *end_x)
{
	Line line = TLINE(y);
	int linelen = tlinelen(y);
	int sx, ex;
	Glyph *gp;

	if (linelen == 0 || x >= linelen)
		return 0;

	gp = &line[x];

	/* If on whitespace, select whitespace run */
	if (iswspace(gp->u)) {
		sx = x;
		ex = x;
		while (sx > 0 && iswspace(line[sx - 1].u))
			sx--;
		while (ex < linelen - 1 && iswspace(line[ex + 1].u))
			ex++;
		*start_x = sx;
		*end_x = ex;
		return 1;
	}

	/* Find WORD start */
	sx = x;
	while (sx > 0 && !iswspace(line[sx - 1].u))
		sx--;

	/* Find WORD end */
	ex = x;
	while (ex < linelen - 1 && !iswspace(line[ex + 1].u))
		ex++;

	if (!inner) {
		/* 'around' - include trailing whitespace, or leading if at end */
		if (ex < linelen - 1 && iswspace(line[ex + 1].u)) {
			while (ex < linelen - 1 && iswspace(line[ex + 1].u))
				ex++;
		} else if (sx > 0 && iswspace(line[sx - 1].u)) {
			while (sx > 0 && iswspace(line[sx - 1].u))
				sx--;
		}
	}

	*start_x = sx;
	*end_x = ex;
	return 1;
}

/* Find matching pair boundaries (quotes, brackets, etc.)
 * If cursor is not inside an enclosing pair, search to the right for one. */
static int
vimnav_find_pair_bounds(int x, int y, Rune open, Rune close, int inner, int *start_x, int *end_x)
{
	Line line = TLINE(y);
	int linelen = tlinelen(y);
	int sx = -1, ex = -1;
	int depth = 0;
	int i;

	if (linelen == 0)
		return 0;

	/* For quotes (open == close), logic is different from brackets */
	if (open == close) {
		/* First, try to find if cursor is inside a quote pair.
		 * Count quotes to the left of cursor to determine if we're inside. */
		int quotes_left = 0;
		for (i = 0; i < x; i++) {
			if (line[i].u == open)
				quotes_left++;
		}

		/* If odd number of quotes to the left, we're inside a quoted region */
		if (quotes_left % 2 == 1) {
			/* Find opening quote (last quote before cursor) */
			for (i = x - 1; i >= 0; i--) {
				if (line[i].u == open) {
					sx = i;
					break;
				}
			}
			/* Find closing quote (first quote at or after cursor) */
			for (i = x; i < linelen; i++) {
				if (line[i].u == close) {
					ex = i;
					break;
				}
			}
			if (sx >= 0 && ex >= 0)
				goto found;
		}

		/* If cursor is on a quote, check if it starts a pair */
		if (line[x].u == open) {
			sx = x;
			for (i = x + 1; i < linelen; i++) {
				if (line[i].u == close) {
					ex = i;
					goto found;
				}
			}
		}

		/* Not inside quotes - search right for a quote pair */
		for (i = x + 1; i < linelen; i++) {
			if (line[i].u == open) {
				sx = i;
				/* Find closing quote */
				for (int j = i + 1; j < linelen; j++) {
					if (line[j].u == close) {
						ex = j;
						goto found;
					}
				}
				/* Opening quote found but no closing - continue searching */
				sx = -1;
			}
		}
		return 0;  /* No valid quote pair found */
	} else {
		/* For brackets: handle nesting */
		/* First, try to find opening bracket to the left (cursor inside pair) */
		depth = 0;
		for (i = x; i >= 0; i--) {
			if (line[i].u == close)
				depth++;
			else if (line[i].u == open) {
				if (depth == 0) {
					sx = i;
					break;
				}
				depth--;
			}
		}

		if (sx >= 0) {
			/* Found opening bracket to the left, find matching closer */
			depth = 0;
			for (i = sx; i < linelen; i++) {
				if (line[i].u == open)
					depth++;
				else if (line[i].u == close) {
					depth--;
					if (depth == 0) {
						ex = i;
						goto found;
					}
				}
			}
			/* Opening found but no valid closing - fall through to search right */
		}

		/* Not inside a pair - search right for an opening bracket */
		for (i = x + 1; i < linelen; i++) {
			if (line[i].u == open) {
				sx = i;
				/* Find matching closing bracket */
				depth = 1;
				for (int j = i + 1; j < linelen; j++) {
					if (line[j].u == open)
						depth++;
					else if (line[j].u == close) {
						depth--;
						if (depth == 0) {
							ex = j;
							goto found;
						}
					}
				}
				/* Opening found but no closing - continue searching */
				sx = -1;
			}
		}
		return 0;  /* No valid pair found */
	}

found:
	if (inner) {
		/* Exclude the delimiters */
		sx++;
		ex--;
		if (sx > ex)
			return 0;  /* Empty inside */
	}

	*start_x = sx;
	*end_x = ex;
	return 1;
}

/* Select text object and enter visual mode */
static void
vimnav_select_textobj(int start_x, int end_x)
{
	int screen_y = vimnav_screen_y();

	/* Enter visual mode with selection */
	vimnav.mode = VIMNAV_VISUAL;
	vimnav.anchor_x = start_x;
	vimnav.anchor_abs_y = screen_y - term.scr;
	vimnav.x = end_x;
	vimnav.savedx = end_x;

	selstart(start_x, screen_y, 0);
	selextend(end_x, screen_y, SEL_REGULAR, 0);
	tfulldirt();
}

/* Handle text object key (w, W, ", (, ), [, ], {, }) after i/a prefix */
static int
vimnav_handle_textobj(ulong ksym, int inner)
{
	int start_x, end_x;
	int y = vimnav_screen_y();
	int found = 0;

	switch (ksym) {
	case 'w':
		found = vimnav_find_word_bounds(vimnav.x, y, inner, &start_x, &end_x);
		break;
	case 'W':
		found = vimnav_find_WORD_bounds(vimnav.x, y, inner, &start_x, &end_x);
		break;
	case '"':
		found = vimnav_find_pair_bounds(vimnav.x, y, '"', '"', inner, &start_x, &end_x);
		break;
	case '\'':
		found = vimnav_find_pair_bounds(vimnav.x, y, '\'', '\'', inner, &start_x, &end_x);
		break;
	case '`':
		found = vimnav_find_pair_bounds(vimnav.x, y, '`', '`', inner, &start_x, &end_x);
		break;
	case '(':
	case ')':
	case 'b':
		found = vimnav_find_pair_bounds(vimnav.x, y, '(', ')', inner, &start_x, &end_x);
		break;
	case '[':
	case ']':
		found = vimnav_find_pair_bounds(vimnav.x, y, '[', ']', inner, &start_x, &end_x);
		break;
	case '{':
	case '}':
	case 'B':
		found = vimnav_find_pair_bounds(vimnav.x, y, '{', '}', inner, &start_x, &end_x);
		break;
	case '<':
	case '>':
		found = vimnav_find_pair_bounds(vimnav.x, y, '<', '>', inner, &start_x, &end_x);
		break;
	default:
		return 0;  /* Unknown text object */
	}

	if (found) {
		vimnav_select_textobj(start_x, end_x);
		return 1;
	}

	return 0;
}

static void
vimnav_move_top(void)
{
	int was_in_prompt_space = vimnav_is_prompt_space(vimnav.y);

	/* Scroll to the oldest populated history row. */
	vimnav_scroll_up(term.histn - term.scr);
	vimnav.y = 0;

	/* Handoff: if we left prompt space with zsh in visual mode, inherit selection */
	if (was_in_prompt_space && !vimnav_is_prompt_space(vimnav.y) &&
	    vimnav.zsh_visual && vimnav.mode == VIMNAV_NORMAL) {
		int prompt_screen_y = term.c.y + term.scr;
		int prompt_end = vimnav_find_prompt_end(prompt_screen_y);
		vimnav.anchor_x = prompt_end + vimnav.zsh_visual_anchor;
		vimnav.anchor_abs_y = term.c.y;  /* Anchor stays on prompt line */
		if (vimnav.zsh_visual_line) {
			vimnav.mode = VIMNAV_VISUAL_LINE;
		} else {
			vimnav.mode = VIMNAV_VISUAL;
		}
	}

	vimnav_restore_saved_column();
	vimnav_update_selection();
}

static void
vimnav_move_bottom(void)
{
	/* Scroll back to show current prompt */
	if (term.scr > 0) {
		kscrolldown(&(Arg){ .i = term.scr });
	}
	vimnav.y = term.c.y;  /* Use current shell cursor position */

	vimnav_restore_saved_column();

	/* Sync cursor to zsh position when entering prompt space */
	vimnav_sync_to_zsh_cursor();
	vimnav_update_selection();
}

/* { - move to previous prompt line (or top of history) */
static void
vimnav_move_prev_prompt(void)
{
	int y;
	int was_in_prompt_space = vimnav_is_prompt_space(vimnav.y);

	/* First, scan within visible screen above cursor */
	for (y = vimnav.y - 1; y >= 0; y--) {
		if (vimnav_has_main_prompt(y))
			goto found;
	}

	/* Not found on screen. Scan into history by increasing term.scr. */
	if (!IS_SET(MODE_ALTSCREEN)) {
		while (term.scr < term.histn) {
			term.scr++;
			/* The new line scrolled in at screen top is TLINE(0) */
			if (vimnav_has_main_prompt(0)) {
				y = 0;
				tfulldirt();
				goto found;
			}
		}
	}

	/* No prompt found above. Move to top of reachable history (like gg). */
	vimnav.y = 0;
	vimnav_restore_saved_column();
	tfulldirt();
	goto handoff;

found:
	vimnav.y = y;
	vimnav_restore_saved_column();
	tfulldirt();

handoff:
	/* Handoff: if we left prompt space with zsh in visual mode, inherit selection */
	if (was_in_prompt_space && !vimnav_is_prompt_space(vimnav.y) &&
	    vimnav.zsh_visual && vimnav.mode == VIMNAV_NORMAL) {
		int prompt_screen_y = term.c.y + term.scr;
		int prompt_end = vimnav_find_prompt_end(prompt_screen_y);
		vimnav.anchor_x = prompt_end + vimnav.zsh_visual_anchor;
		vimnav.anchor_abs_y = term.c.y;
		if (vimnav.zsh_visual_line) {
			vimnav.mode = VIMNAV_VISUAL_LINE;
		} else {
			vimnav.mode = VIMNAV_VISUAL;
		}
	}

	vimnav_update_selection();
}

/* } - move to next prompt line (or current prompt) */
static void
vimnav_move_next_prompt(void)
{
	int y;
	int max_valid_y = term.scr + term.c.y;

	/* Scan downward from current position */
	for (y = vimnav.y + 1; y <= max_valid_y; y++) {
		if (vimnav_has_main_prompt(y))
			goto found;
	}

	/* No prompt found below. Go to current active prompt (like G). */
	if (term.scr > 0)
		kscrolldown(&(Arg){ .i = term.scr });
	vimnav.y = term.c.y;
	vimnav.savedx = 0;
	vimnav_sync_to_zsh_cursor();
	vimnav_update_selection();
	return;

found:
	/* If target is below visible screen, scroll to bring it into view */
	if (y >= term.row) {
		int scroll_amount = y - (term.row - 1);
		if (scroll_amount > term.scr)
			scroll_amount = term.scr;
		kscrolldown(&(Arg){ .i = scroll_amount });
		y -= scroll_amount;
	}

	vimnav.y = y;
	vimnav_restore_saved_column();

	/* If we landed in the current prompt space, sync to zsh cursor */
	if (vimnav_is_prompt_space(vimnav.y))
		vimnav_sync_to_zsh_cursor();

	tfulldirt();
	vimnav_update_selection();
}

/* H - move cursor to top line of current screen */
static void
vimnav_move_screen_top(void)
{
	vimnav.y = 0;
	vimnav_restore_saved_column();
	vimnav_update_selection();
}

/* L - move cursor to last visible line on screen */
static void
vimnav_move_screen_bottom(void)
{
	int bottom_y;

	if (vimnav.forced && IS_SET(MODE_ALTSCREEN)) {
		/* Forced mode on alt screen: full screen is navigable */
		bottom_y = term.row - 1;
	} else {
		/* Prompt's screen row, clamped to screen bounds */
		bottom_y = MIN(term.c.y + term.scr, term.row - 1);
	}

	vimnav.y = bottom_y;
	vimnav_restore_saved_column();

	/* Sync cursor to zsh position if we landed in prompt space */
	if (vimnav_is_prompt_space(vimnav.y))
		vimnav_sync_to_zsh_cursor();
	vimnav_update_selection();
}

/* M - move cursor to middle line between top and prompt */
static void
vimnav_move_screen_middle(void)
{
	int bottom_y;

	if (vimnav.forced && IS_SET(MODE_ALTSCREEN)) {
		/* Forced mode on alt screen: full screen is navigable */
		bottom_y = term.row - 1;
	} else {
		/* Prompt's screen row, clamped to screen bounds */
		bottom_y = MIN(term.c.y + term.scr, term.row - 1);
	}

	vimnav.y = bottom_y / 2;
	vimnav_restore_saved_column();
	vimnav_update_selection();
}

/* Ctrl+1-9,0,-: move cursor to percent of visible screen */
static void
vimnav_move_screen_percent(int percent)
{
	int target_y;

	target_y = ((term.row - 1) * percent + 50) / 100;

	/* Clamp to prompt line when not in forced mode */
	if (!vimnav.forced) {
		int prompt_y = MIN(term.c.y + term.scr, term.row - 1);
		if (target_y > prompt_y)
			target_y = prompt_y;
	}

	vimnav.y = target_y;
	vimnav_restore_saved_column();

	if (vimnav_is_prompt_space(vimnav.y))
		vimnav_sync_to_zsh_cursor();
	vimnav_update_selection();
}

static void
vimnav_toggle_visual_char(void)
{
	int was_block = vimnav.mode == VIMNAV_VISUAL_BLOCK;

	if (vimnav.mode == VIMNAV_VISUAL) {
		vimnav.mode = VIMNAV_NORMAL;
		vimnav_notify_zsh_visual_end();
		selclear();
		vimnav_sync_to_zsh_cursor();
	} else {
		/* Switching from another visual mode: keep anchor */
		if (vimnav.mode != VIMNAV_VISUAL_LINE && vimnav.mode != VIMNAV_VISUAL_BLOCK) {
			vimnav.anchor_x = vimnav.x;
			vimnav.anchor_abs_y = vimnav_screen_y() - term.scr;
		}
		vimnav.mode = VIMNAV_VISUAL;
		if (was_block)
			vimnav_restore_saved_column();
		selstart(vimnav.anchor_x, vimnav.anchor_abs_y + term.scr, 0);
		selextend(vimnav.x, vimnav_screen_y(), SEL_REGULAR, 0);
	}
	tfulldirt();
}

static void
vimnav_toggle_visual_line(void)
{
	int screen_y = vimnav_screen_y();
	int was_block = vimnav.mode == VIMNAV_VISUAL_BLOCK;

	if (vimnav.mode == VIMNAV_VISUAL_LINE) {
		vimnav.mode = VIMNAV_NORMAL;
		vimnav_notify_zsh_visual_end();
		selclear();
		vimnav_sync_to_zsh_cursor();
	} else {
		/* Switching from another visual mode: keep anchor y */
		if (vimnav.mode != VIMNAV_VISUAL && vimnav.mode != VIMNAV_VISUAL_BLOCK)
			vimnav.anchor_abs_y = screen_y - term.scr;
		vimnav.mode = VIMNAV_VISUAL_LINE;
		if (was_block)
			vimnav_restore_saved_column();
		selstart(0, vimnav.anchor_abs_y + term.scr, 0);
		sel.snap = SNAP_LINE;
		selextend(term.col - 1, screen_y, SEL_REGULAR, 0);
	}
	tfulldirt();
}

static void
vimnav_toggle_visual_block(void)
{
	if (vimnav.mode == VIMNAV_VISUAL_BLOCK) {
		vimnav.mode = VIMNAV_NORMAL;
		vimnav_restore_saved_column();
		vimnav_notify_zsh_visual_end();
		selclear();
		vimnav_sync_to_zsh_cursor();
	} else {
		/* Switching from another visual mode: keep anchor */
		if (vimnav.mode != VIMNAV_VISUAL && vimnav.mode != VIMNAV_VISUAL_LINE) {
			vimnav.anchor_x = vimnav.x;
			vimnav.anchor_abs_y = vimnav_screen_y() - term.scr;
		}
		vimnav.mode = VIMNAV_VISUAL_BLOCK;
		selstart(vimnav.anchor_x, vimnav.anchor_abs_y + term.scr, 0);
		sel.type = SEL_RECTANGULAR;
		selextend(vimnav.x, vimnav_screen_y(), SEL_RECTANGULAR, 0);
	}
	tfulldirt();
}

int
tisvimnav_paste(void)
{
	return vimnav_paste_strip_newlines;
}

void
vimnav_paste_done(void)
{
	vimnav_paste_strip_newlines = 0;
	if (vimnav_paste_after_cursor) {
		/* Return to vicmd mode after paste */
		ttywrite("\033", 1, 1);
		vimnav_paste_after_cursor = 0;
	}
}

/* Strip trailing newline from single-line text (character-wise copy) */
static void
yank_strip_trailing_newline(char *text)
{
	size_t len;

	if (!text)
		return;
	len = strlen(text);
	if (len > 0 && text[len - 1] == '\n' && !memchr(text, '\n', len - 1))
		text[len - 1] = '\0';
}

static void
vimnav_yank_selection(void)
{
	static const char object_replacement[] = "\xef\xbf\xbc";
	unsigned char *png;
	size_t png_length = 0;
	char *text = getsel();

	png = getselimage(&png_length);
	if (text)
		yank_strip_trailing_newline(text);
	/* Text-only clipboard consumers still get one semantic character rather
	 * than an empty line for an image-only selection. */
	if (png && (!text || !text[0])) {
		if (!text)
			text = xstrdup(object_replacement);
		else
			memcpy(text, object_replacement, sizeof(object_replacement));
	}
	if (text)
		xsetsel(text);
	if (png)
		xsetimage(png, png_length);
	if (text || png)
		xclipcopy();
}

static int
vimnav_find_prompt_end(int screen_y)
{
	/* Find the end of the prompt on this line by looking for common prompt
	 * delimiters: '% ', '$ ', '> ', '# ' (with trailing space).
	 * Returns the x position after the delimiter, or 0 if not found. */
	Line line = TLINE(screen_y);
	int linelen = tlinelen(screen_y);
	int last_delim = -1;

	for (int i = 0; i < linelen && i + 1 < term.col; i++) {
		Rune c = line[i].u;
		Rune next = line[i + 1].u;
		if ((c == '%' || c == '$' || c == '>' || c == '#') && next == ' ') {
			last_delim = i + 2;  /* Position after "% " */
		}
	}

	return last_delim > 0 ? last_delim : 0;
}

static int
vimnav_has_main_prompt(int screen_y)
{
	/* Check if this line has a main prompt (not a continuation prompt).
	 * Main prompts typically have '% ' or '$ ' or '# ' at the start or after path.
	 * Continuation prompts are just '> ' at the start.
	 * For '%', require it to be preceded by ']' or at position 0 to avoid
	 * false positives from command output (e.g. "25% /" in df output). */
	Line line = TLINE(screen_y);
	int linelen = tlinelen(screen_y);

	for (int i = 0; i < linelen && i + 1 < term.col; i++) {
		Rune c = line[i].u;
		Rune next = line[i + 1].u;
		if (c == '%' && next == ' ' && (i == 0 || line[i - 1].u == ']')) {
			return 1;
		}
		if ((c == '$' || c == '#') && next == ' ') {
			return 1;
		}
	}
	return 0;
}

static int
vimnav_find_prompt_start_y(void)
{
	/* Find the screen row where the prompt starts by searching upward
	 * from the prompt's screen row for a line with a main prompt delimiter.
	 * For multi-line commands, this finds the first line of the command.
	 * Only searches within term.line[] (screen rows >= term.scr). */
	int y;
	int prompt_y = term.c.y + term.scr;

	for (y = prompt_y; y >= term.scr; y--) {
		if (vimnav_has_main_prompt(y)) {
			return y;
		}
	}
	/* If no prompt found, assume prompt's screen row */
	return prompt_y;
}

static int
vimnav_is_prompt_space(int y)
{
	/* Check if screen row y is within the prompt space (multi-line command region).
	 * The prompt is at screen row (term.c.y + term.scr). It's visible when
	 * that row is within the screen bounds. Returns 1 if y is between
	 * prompt_start and the prompt's screen row (inclusive). */
	int prompt_start;
	int prompt_end_y;

	if (vimnav_terminal_owned()) {
		return 0;  /* Terminal-owned modes have no prompt space concept */
	}

	prompt_end_y = term.c.y + term.scr;
	if (prompt_end_y >= term.row) {
		return 0;  /* Prompt scrolled off screen */
	}

	prompt_start = vimnav_find_prompt_start_y();
	return (y >= prompt_start && y <= prompt_end_y);
}

static char *
vimnav_build_prompt_text(int start_y, int end_y)
{
	/* Build text from prompt lines, stripping the prompt prefix on each.
	 * For multiline commands this strips '% ', '> ', etc. from each line
	 * so the yanked text is just the command. Wrapped rows (ATTR_WRAP) are
	 * joined without newlines and their content is taken raw (no prompt
	 * stripping, since they're overflow from the previous row). */
	char *buf, *ptr;
	int y, x, start_x, linelen, wrapped;
	Line line;

	buf = xmalloc((term.col + 1) * (end_y - start_y + 1) * 4);
	ptr = buf;
	wrapped = 0;

	for (y = start_y; y <= end_y; y++) {
		/* Only strip prompt prefix on real line starts, not wrapped rows */
		start_x = wrapped ? 0 : vimnav_find_prompt_end(y);
		linelen = tlinelen(y);

		if (start_x < linelen) {
			line = TLINE(y);
			for (x = start_x; x < linelen; x++) {
				if (line[x].mode & ATTR_WDUMMY)
					continue;
				ptr += utf8encode(line[x].u, ptr);
			}
		}

		/* ATTR_WRAP on the last cell means this row wraps to the next */
		wrapped = TLINE(y)[term.col - 1].mode & ATTR_WRAP;

		/* Add newline between real lines, not wrapped continuations */
		if (y < end_y && !wrapped)
			*ptr++ = '\n';
	}
	*ptr = '\0';

	return buf;
}

static void
vimnav_yank_line(void)
{
	int screen_y = vimnav_screen_y();

	/* Check if cursor is in prompt space */
	if (term.scr == 0 && vimnav_is_prompt_space(screen_y)) {
		int prompt_start_y = vimnav_find_prompt_start_y();
		int prompt_end_y = term.c.y;  /* term.scr == 0 */
		char *text;

		/* Build text from all prompt lines, stripping prompt prefixes */
		text = vimnav_build_prompt_text(prompt_start_y, prompt_end_y);
		if (text && text[0]) {
			xsetsel(text);
			xclipcopy();
		} else {
			free(text);
		}
	} else {
		/* For non-prompt lines, use SNAP_LINE to select whole line */
		selstart(0, screen_y, 0);
		sel.snap = SNAP_LINE;
		sel.mode = SEL_READY;  /* Required for selextend to work with done=1 */
		selextend(term.col - 1, screen_y, SEL_REGULAR, 1);
		vimnav_yank_selection();
		selclear();
	}

	tfulldirt();
}

void
vimnav_enter(void)
{
	int prompt_end;

	if (vimnav.mode != VIMNAV_INACTIVE || IS_SET(MODE_ALTSCREEN))
		return;

	/* If scrolled away from prompt, scroll back first for clean state */
	if (term.scr > 0) {
		kscrolldown(&(Arg){ .i = term.scr });
	}

	vimnav.y = term.c.y;  /* Screen row */
	vimnav.prompt_y = term.c.y;  /* Can't go below this row when scr == 0 */
	vimnav.scr_at_entry = term.scr;  /* Track scroll position at entry (should be 0) */
	vimnav.pending_textobj = 0;  /* Clear any pending text object state */
	vimnav.pending_find = 0;     /* Clear any pending find state */
	vimnav.pending_g = 0;        /* Clear any pending g state */

	/* Use zsh-reported cursor position for x coordinate */
	prompt_end = vimnav_find_prompt_end(vimnav.y);
	vimnav.x = prompt_end + vimnav.zsh_cursor;
	vimnav.savedx = vimnav.x;
	vimnav.ox = vimnav.x;
	vimnav.oy = vimnav.y;
	vimnav.last_shell_x = vimnav.x;

	/* Check if zsh was in visual mode - if so, inherit the selection */
	if (vimnav.zsh_visual) {
		if (vimnav.zsh_visual_line) {
			vimnav.mode = VIMNAV_VISUAL_LINE;
		} else {
			vimnav.mode = VIMNAV_VISUAL;
		}
		/* Set anchor to zsh's visual anchor position */
		vimnav.anchor_x = prompt_end + vimnav.zsh_visual_anchor;
		vimnav.anchor_abs_y = vimnav.y - term.scr;  /* Anchor is on prompt line */
		vimnav_update_selection();
	} else {
		vimnav.mode = VIMNAV_NORMAL;
		selclear();
	}

	tfulldirt();
}

void
vimnav_force_enter(void)
{
	if (vimnav.mode != VIMNAV_INACTIVE)
		return;

	vimnav.y = term.c.y;
	vimnav.x = term.c.x;
	vimnav.savedx = vimnav.x;
	vimnav.ox = vimnav.x;
	vimnav.oy = vimnav.y;
	vimnav.prompt_y = term.c.y;
	vimnav.scr_at_entry = term.scr;
	vimnav.pending_textobj = 0;
	vimnav.pending_find = 0;
	vimnav.pending_g = 0;
	vimnav.last_shell_x = vimnav.x;
	vimnav.forced = 1;

	vimnav.mode = VIMNAV_NORMAL;
	selclear();
	tfulldirt();
}

void
vimnav_exit(void)
{
	if (vimnav.mode == VIMNAV_INACTIVE)
		return;

	vimnav.mode = VIMNAV_INACTIVE;
	vimnav.pending_textobj = 0;
	vimnav.pending_find = 0;
	vimnav.pending_g = 0;
	vimnav.forced = 0;
	selclear();
	tfulldirt();
}

int
vimnav_handle_key(ulong ksym, uint state)
{
	int handled = 1;

	/* Ignore modifier-only key presses (Shift, Ctrl, Alt, Super) */
	if (ksym >= 0xffe1 && ksym <= 0xffee)  /* XK_Shift_L to XK_Hyper_R */
		return 1;

	/* Handle pending text object (after 'i' or 'a' was pressed) */
	if (vimnav.pending_textobj) {
		int inner = (vimnav.pending_textobj == 'i');
		vimnav.pending_textobj = 0;  /* Clear pending state */
		if (vimnav_handle_textobj(ksym, inner))
			return 1;
		/* If text object not found, fall through to normal handling */
	}

	/* Handle pending find (after 'f' or 'F' was pressed) */
	if (vimnav.pending_find) {
		int forward = (vimnav.pending_find == 'f');
		vimnav.pending_find = 0;  /* Clear pending state */
		/* Store the search for repeat with ;/, */
		vimnav.last_find_char = ksym;
		vimnav.last_find_forward = forward;
		vimnav_find_char(ksym, forward);
		return 1;
	}

	/* Handle pending g (for gg command) */
	if (vimnav.pending_g) {
		vimnav.pending_g = 0;  /* Clear pending state */
		if (ksym == 'g') {
			vimnav_move_top();
			return 1;
		}
		/* Any other key after g - ignore the g and process the key normally */
	}

	/* Handle Ctrl+scroll commands */
	if (state & ControlMask) {
		int max_valid_y;
		int old_scr, old_y, old_x;
		switch (ksym) {
		case 'e':
			old_scr = term.scr;
			old_y = vimnav.y;
			old_x = vimnav.x;
			vimnav_scroll_down(1);
			if (term.scr < old_scr) {
				/* Scroll happened - move cursor up to keep it on the same content line */
				if (vimnav.y > 0) {
					vimnav.y--;
					vimnav_restore_saved_column();
					vimnav_update_selection();
				}
			} else {
				/* Couldn't scroll - undo any cursor movement from scroll_down */
				vimnav.y = old_y;
				vimnav.x = old_x;
			}
			return 1;
		case 'y':
			old_scr = term.scr;
			old_y = vimnav.y;
			old_x = vimnav.x;
			vimnav_scroll_up(1);
			if (term.scr > old_scr) {
				/* Scroll happened - move cursor down to keep it on the same content line */
				max_valid_y = term.scr + term.c.y;
				if (vimnav.y < max_valid_y && vimnav.y < term.row - 1) {
					vimnav.y++;
					vimnav_restore_saved_column();
					vimnav_update_selection();
				}
			} else {
				/* Couldn't scroll - undo any cursor movement from scroll_up */
				vimnav.y = old_y;
				vimnav.x = old_x;
			}
			return 1;
		case 'u':
			vimnav_scroll_up(term.row / 2 - 1);
			return 1;
		case 'd':
			vimnav_scroll_down(term.row / 2 - 1);
			return 1;
		case 'b':
			vimnav_scroll_up(term.row - 4);
			/* Move cursor to bottom of screen */
			max_valid_y = term.scr + term.c.y;
			vimnav.y = MIN(term.row - 1, max_valid_y);
			vimnav_restore_saved_column();
			vimnav_update_selection();
			return 1;
		case 'f':
			vimnav_scroll_down(term.row - 4);
			/* Move cursor to top of screen */
			vimnav.y = 0;
			vimnav_restore_saved_column();
			vimnav_update_selection();
			return 1;
		case 'v':
			/* Unlike character visual mode, block selection is owned by st even
			 * on the live prompt. It should behave like forced navigation, not
			 * turn into a zsh history/editing operation. */
			vimnav_toggle_visual_block();
			return 1;
		case '1': vimnav_move_screen_percent(0);   return 1;
		case '2': vimnav_move_screen_percent(10);  return 1;
		case '3': vimnav_move_screen_percent(20);  return 1;
		case '4': vimnav_move_screen_percent(30);  return 1;
		case '5': vimnav_move_screen_percent(40);  return 1;
		case '6': vimnav_move_screen_percent(50);  return 1;
		case '7': vimnav_move_screen_percent(60);  return 1;
		case '8': vimnav_move_screen_percent(70);  return 1;
		case '9': vimnav_move_screen_percent(80);  return 1;
		case '0': vimnav_move_screen_percent(90);  return 1;
		case '-': vimnav_move_screen_percent(100); return 1;
		default:
			return 0;  /* Unknown Ctrl key, exit vim mode */
		}
	}

	/* Only handle unmodified keys or Shift */
	if (state & ~ShiftMask)
		return 0;

	switch (ksym) {
	/* Movement keys */
	case 'h':
		/* In prompt space: always pass to zsh (zsh has cursor authority) */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;  /* Pass through to zsh */
		}
		vimnav_move_left();
		break;
	case 'j':
		vimnav_move_down();
		break;
	case 'k':
		vimnav_move_up();
		break;
	case 'l':
		/* In prompt space: always pass to zsh (zsh has cursor authority) */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;  /* Pass through to zsh */
		}
		vimnav_move_right();
		break;
	case '0':
		/* In prompt space: always pass to zsh (zsh has cursor authority) */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;  /* Pass through to zsh */
		}
		vimnav_move_bol();
		break;
	case '$':
	case 'w':
	case 'b':
	case 'e':
	case 'W':
	case 'B':
	case 'E':
		/* In prompt space: always pass to zsh (zsh has cursor authority) */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;  /* Pass through to zsh */
		}
		if (ksym == '$')
			vimnav_move_eol();
		else if (ksym == 'w')
			vimnav_move_word_forward();
		else if (ksym == 'b')
			vimnav_move_word_backward();
		else if (ksym == 'e')
			vimnav_move_word_end();
		else if (ksym == 'W')
			vimnav_move_WORD_forward();
		else if (ksym == 'B')
			vimnav_move_WORD_backward();
		else if (ksym == 'E')
			vimnav_move_WORD_end();
		break;
	case 'g':
		vimnav.pending_g = 1;  /* Wait for second g (gg command) */
		break;
	case 'G':
		vimnav_move_bottom();
		break;
	case 'H':
		vimnav_move_screen_top();
		break;
	case 'L':
		vimnav_move_screen_bottom();
		break;
	case 'M':
		vimnav_move_screen_middle();
		break;

	/* Jump to previous/next prompt line ({/}) */
	case '{':
		vimnav_move_prev_prompt();
		break;
	case '}':
		vimnav_move_next_prompt();
		break;

	/* Command-line mode */
	case ':':
		cmdline_open();
		break;

	/* Search */
	case '/':
		cmdline_open_search(1);
		break;
	case '?':
		cmdline_open_search(0);
		break;
	case 'n':
		if (search_has_pattern())
			search_next(1);
		else if (!vimnav_is_visual())
			handled = 0;
		break;
	case 'N':
		if (search_has_pattern())
			search_next(-1);
		else if (!vimnav_is_visual())
			handled = 0;
		break;

	/* Find character on line (f/F) */
	case 'f':
	case 'F':
		/* In prompt space: pass to zsh */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;
		}
		vimnav.pending_find = ksym;
		break;

	/* Repeat find (;/,) */
	case ';':
		/* In prompt space: pass to zsh */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;
		}
		if (vimnav.last_find_char) {
			vimnav_find_char(vimnav.last_find_char, vimnav.last_find_forward);
		}
		break;
	case ',':
		/* In prompt space: pass to zsh */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;
		}
		if (vimnav.last_find_char) {
			vimnav_find_char(vimnav.last_find_char, !vimnav.last_find_forward);
		}
		break;

	/* Visual mode */
	case 'v':
		/* In prompt space: pass to zsh (zsh handles char selection) */
		if (vimnav_is_prompt_space(vimnav.y)) {
			return 0;  /* Pass through to zsh */
		}
		vimnav_toggle_visual_char();
		break;
	case 'V':
		/* Line visual always handled by st (enters nav mode with line selection) */
		vimnav_toggle_visual_line();
		break;

	/* Text object prefix: 'i' for inner, 'a' for around (only in visual mode) */
	case 'i':
	case 'a':
		/* In visual mode: start text object sequence */
		if (vimnav_is_visual()) {
			vimnav.pending_textobj = ksym;
			break;
		}
		/* In forced mode: exit nav mode (return to app) */
		if (vimnav.forced) {
			vimnav_exit();
			break;
		}
		/* In normal mode: snap to prompt and pass to zsh (insert/append) */
		vimnav_snap_to_prompt();
		return 0;

	/* Editing operations - snap to prompt and pass to zsh */
	case 'x':  /* delete char */
	case 'X':  /* delete char before */
	case 'd':  /* delete with motion */
	case 'D':  /* delete to end of line */
	case 'c':  /* change with motion */
	case 'C':  /* change to end of line */
	case 's':  /* substitute char */
	case 'S':  /* substitute line */
	case 'r':  /* replace char */
	case 'R':  /* replace mode */
	case 'A':  /* append at end of line */
	case 'I':  /* insert at beginning of line */
	case 'o':  /* open line below */
	case 'O':  /* open line above */
	case 'u':  /* undo */
	case '.':  /* repeat last command */
	case '~':  /* toggle case */
	case 'J':  /* zsh history navigation (user-bound) */
	case 'K':  /* zsh history navigation (user-bound) */
		/* Terminal-owned modes must not jump to or mutate shell history. */
		if (vimnav_terminal_owned())
			break;
		vimnav_snap_to_prompt();
		return 0;  /* Pass through to zsh */

	/* Yank */
	case 'y':
		if (vimnav_is_visual()) {
			int was_block = vimnav.mode == VIMNAV_VISUAL_BLOCK;

			/* In visual mode, yank selection */
			vimnav_yank_selection();
			vimnav.mode = VIMNAV_NORMAL;
			if (was_block)
				vimnav_restore_saved_column();
			vimnav_notify_zsh_visual_end();  /* Tell zsh to exit visual mode */
			selclear();
			vimnav_sync_to_zsh_cursor();  /* Sync cursor if in prompt space */
			tfulldirt();
		} else if (vimnav.zsh_visual) {
			/* zsh is in visual mode on prompt line - yank and clear */
			vimnav_yank_selection();
			vimnav_notify_zsh_visual_end();  /* Tell zsh to exit visual mode */
			selclear();
			tfulldirt();
		} else {
			/* No selection: yank line (or just command on prompt line) */
			vimnav_yank_line();
		}
		break;

	/* Paste after cursor (vim-style 'p') */
	case 'p':
		/* Terminal-owned modes have no shell target to paste into. */
		if (vimnav_terminal_owned())
			break;
		vimnav_snap_to_prompt();
		vimnav_paste_strip_newlines = 1;
		vimnav_paste_after_cursor = 1;
		ttywrite("a", 1, 1);  /* Enter insert mode after cursor */
		clippaste(NULL);
		break;

	/* Escape: clear visual selection or stay in normal mode */
	case 0xff1b: /* XK_Escape */
		if (vimnav_is_visual()) {
			int was_block = vimnav.mode == VIMNAV_VISUAL_BLOCK;

			vimnav.mode = VIMNAV_NORMAL;
			if (was_block)
				vimnav_restore_saved_column();
			if (!vimnav.forced)
				vimnav_notify_zsh_visual_end();  /* Tell zsh to exit visual mode */
			selclear();
			if (!vimnav.forced)
				vimnav_sync_to_zsh_cursor();  /* Sync cursor if in prompt space */
			tfulldirt();
		} else if (vimnav.zsh_visual) {
			/* zsh is in visual mode on prompt line - clear st's rendering and notify zsh */
			vimnav_notify_zsh_visual_end();  /* Sends Escape to zsh and clears flag */
			selclear();
			tfulldirt();
		}
		break;

	default:
		/* In visual mode, consume unrecognized keys to prevent them
		 * from leaking to zsh and leaving a ghost selection */
		if (vimnav_is_visual())
			handled = 1;
		else
			handled = 0;
		break;
	}

	return handled;
}
